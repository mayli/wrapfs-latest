/*
 * Copyright (c) 2003-2007 Erez Zadok
 * Copyright (c) 2003-2006 Charles P. Wright
 * Copyright (c) 2005-2007 Josef 'Jeff' Sipek
 * Copyright (c) 2005-2006 Junjiro Okajima
 * Copyright (c) 2006      Shaya Potter
 * Copyright (c) 2005      Arun M. Krishnakumar
 * Copyright (c) 2004-2006 David P. Quigley
 * Copyright (c) 2003-2004 Mohammad Nayyer Zubair
 * Copyright (c) 2003      Puja Gupta
 * Copyright (c) 2003      Harikesavan Krishnan
 * Copyright (c) 2003-2007 Stony Brook University
 * Copyright (c) 2003-2007 The Research Foundation of SUNY
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "union.h"

/*
 * Unionfs doesn't implement ->writepages, which is OK with the VFS and
 * keeps our code simpler and smaller.  Nevertheless, somehow, our own
 * ->writepage must be called so we can sync the upper pages with the lower
 * pages: otherwise data changed at the upper layer won't get written to the
 * lower layer.
 *
 * Some lower file systems (e.g., NFS) expect the VFS to call its writepages
 * only, which in turn will call generic_writepages and invoke each of the
 * lower file system's ->writepage.  NFS in particular uses the
 * wbc->fs_private field in its nfs_writepage, which is set in its
 * nfs_writepages.  So if we don't call the lower nfs_writepages first, then
 * NFS's nfs_writepage will dereference a NULL wbc->fs_private and cause an
 * OOPS.  If, however, we implement a unionfs_writepages and then we do call
 * the lower nfs_writepages, then we "lose control" over the pages we're
 * trying to write to the lower file system: we won't be writing our own
 * new/modified data from the upper pages to the lower pages, and any
 * mmap-based changes are lost.
 *
 * This is a fundamental cache-coherency problem in Linux.  The kernel isn't
 * able to support such stacking abstractions cleanly.  One possible clean
 * way would be that a lower file system's ->writepage method have some sort
 * of a callback to validate if any upper pages for the same file+offset
 * exist and have newer content in them.
 *
 * This whole NULL ptr dereference is triggered at the lower file system
 * (NFS) because the wbc->for_writepages is set to 1.  Therefore, to avoid
 * this NULL pointer dereference, we set this flag to 0 and restore it upon
 * exit.  This probably means that we're slightly less efficient in writing
 * pages out, doing them one at a time, but at least we avoid the oops until
 * such day as Linux can better support address_space_ops in a stackable
 * fashion.
 */
int unionfs_writepage(struct page *page, struct writeback_control *wbc)
{
	int err = -EIO;
	struct inode *inode;
	struct inode *lower_inode;
	struct page *lower_page;
	char *kaddr, *lower_kaddr;
	int saved_for_writepages = wbc->for_writepages;

	inode = page->mapping->host;
	lower_inode = unionfs_lower_inode(inode);

	/* find lower page (returns a locked page) */
	lower_page = grab_cache_page(lower_inode->i_mapping, page->index);
	if (!lower_page)
		goto out;

	/* get page address, and encode it */
	kaddr = kmap(page);
	lower_kaddr = kmap(lower_page);

	memcpy(lower_kaddr, kaddr, PAGE_CACHE_SIZE);

	kunmap(page);
	kunmap(lower_page);

	BUG_ON(!lower_inode->i_mapping->a_ops->writepage);

	/* workaround for some lower file systems: see big comment on top */
	if (wbc->for_writepages && !wbc->fs_private)
		wbc->for_writepages = 0;

	/* call lower writepage (expects locked page) */
	err = lower_inode->i_mapping->a_ops->writepage(lower_page, wbc);
	wbc->for_writepages = saved_for_writepages; /* restore value */

	/*
	 * update mtime and ctime of lower level file system
	 * unionfs' mtime and ctime are updated by generic_file_write
	 */
	lower_inode->i_mtime = lower_inode->i_ctime = CURRENT_TIME;
	/* b/c grab_cache_page increased refcnt */
	page_cache_release(lower_page);

	if (err)
		ClearPageUptodate(page);
	else
		SetPageUptodate(page);

out:
	unlock_page(page);
	return err;
}

/*
 * readpage is called from generic_page_read and the fault handler.
 * If your file system uses generic_page_read for the read op, it
 * must implement readpage.
 *
 * Readpage expects a locked page, and must unlock it.
 */
static int unionfs_do_readpage(struct file *file, struct page *page)
{
	int err = -EIO;
	struct dentry *dentry;
	struct file *lower_file = NULL;
	struct inode *inode, *lower_inode;
	char *page_data;
	struct page *lower_page;
	char *lower_page_data;

	dentry = file->f_dentry;
	if (UNIONFS_F(file) == NULL) {
		err = -ENOENT;
		goto out_err;
	}

	lower_file = unionfs_lower_file(file);
	inode = dentry->d_inode;
	lower_inode = unionfs_lower_inode(inode);

	lower_page = NULL;

	/* find lower page (returns a locked page) */
	lower_page = read_cache_page(lower_inode->i_mapping,
				     page->index,
				     (filler_t *) lower_inode->i_mapping->
				     a_ops->readpage, (void *)lower_file);

	if (IS_ERR(lower_page)) {
		err = PTR_ERR(lower_page);
		lower_page = NULL;
		goto out_release;
	}

	/*
	 * wait for the page data to show up
	 * (signaled by readpage as unlocking the page)
	 */
	wait_on_page_locked(lower_page);
	if (!PageUptodate(lower_page)) {
		/*
		 * call readpage() again if we returned from wait_on_page
		 * with a page that's not up-to-date; that can happen when a
		 * partial page has a few buffers which are ok, but not the
		 * whole page.
		 */
		lock_page(lower_page);
		err = lower_inode->i_mapping->a_ops->readpage(lower_file,
							      lower_page);
		if (err) {
			lower_page = NULL;
			goto out_release;
		}

		wait_on_page_locked(lower_page);
		if (!PageUptodate(lower_page)) {
			err = -EIO;
			goto out_release;
		}
	}

	/* map pages, get their addresses */
	page_data = (char *)kmap(page);
	lower_page_data = (char *)kmap(lower_page);

	memcpy(page_data, lower_page_data, PAGE_CACHE_SIZE);

	err = 0;

	kunmap(lower_page);
	kunmap(page);

out_release:
	if (lower_page)
		page_cache_release(lower_page);	/* undo read_cache_page */

	if (err == 0)
		SetPageUptodate(page);
	else
		ClearPageUptodate(page);

out_err:
	return err;
}

int unionfs_readpage(struct file *file, struct page *page)
{
	int err;

	unionfs_read_lock(file->f_dentry->d_sb);
	unionfs_check_file(file);
	if ((err = unionfs_file_revalidate(file, 0)))
		goto out;

	err = unionfs_do_readpage(file, page);

	if (!err)
		touch_atime(unionfs_lower_mnt(file->f_path.dentry),
			    unionfs_lower_dentry(file->f_path.dentry));

	/*
	 * we have to unlock our page, b/c we _might_ have gotten a locked
	 * page.  but we no longer have to wakeup on our page here, b/c
	 * UnlockPage does it
	 */
out:
	unlock_page(page);
	unionfs_check_file(file);
	unionfs_read_unlock(file->f_dentry->d_sb);

	return err;
}

int unionfs_prepare_write(struct file *file, struct page *page, unsigned from,
			  unsigned to)
{
	int err;

	unionfs_read_lock(file->f_dentry->d_sb);
	unionfs_check_file(file);
	err = unionfs_file_revalidate(file, 1);
	unionfs_check_file(file);
	unionfs_read_unlock(file->f_dentry->d_sb);

	return err;
}

int unionfs_commit_write(struct file *file, struct page *page, unsigned from,
			 unsigned to)
{
	int err = -ENOMEM;
	struct inode *inode, *lower_inode;
	struct file *lower_file = NULL;
	loff_t pos;
	unsigned bytes = to - from;
	char *page_data = NULL;
	mm_segment_t old_fs;

	BUG_ON(file == NULL);

	unionfs_read_lock(file->f_dentry->d_sb);
	unionfs_check_file(file);
	if ((err = unionfs_file_revalidate(file, 1)))
		goto out;

	inode = page->mapping->host;
	lower_inode = unionfs_lower_inode(inode);

	if (UNIONFS_F(file) != NULL)
		lower_file = unionfs_lower_file(file);

	/* FIXME: is this assertion right here? */
	BUG_ON(lower_file == NULL);

	page_data = (char *)kmap(page);
	lower_file->f_pos = (page->index << PAGE_CACHE_SHIFT) + from;

	/*
	 * SP: I use vfs_write instead of copying page data and the
	 * prepare_write/commit_write combo because file system's like
	 * GFS/OCFS2 don't like things touching those directly,
	 * calling the underlying write op, while a little bit slower, will
	 * call all the FS specific code as well
	 */
	old_fs = get_fs();
	set_fs(KERNEL_DS);
	err = vfs_write(lower_file, page_data + from, bytes,
			&lower_file->f_pos);
	set_fs(old_fs);

	kunmap(page);

	if (err < 0)
		goto out;

	inode->i_blocks = lower_inode->i_blocks;
	/* we may have to update i_size */
	pos = ((loff_t) page->index << PAGE_CACHE_SHIFT) + to;
	if (pos > i_size_read(inode))
		i_size_write(inode, pos);

	/*
	 * update mtime and ctime of lower level file system
	 * unionfs' mtime and ctime are updated by generic_file_write
	 */
	lower_inode->i_mtime = lower_inode->i_ctime = CURRENT_TIME;

	mark_inode_dirty_sync(inode);

out:
	if (err < 0)
		ClearPageUptodate(page);

	unionfs_read_unlock(file->f_dentry->d_sb);
	unionfs_check_file(file);
	return err;		/* assume all is ok */
}

void unionfs_sync_page(struct page *page)
{
	struct inode *inode;
	struct inode *lower_inode;
	struct page *lower_page;
	struct address_space *mapping;

	inode = page->mapping->host;
	lower_inode = unionfs_lower_inode(inode);

	/* find lower page (returns a locked page) */
	lower_page = grab_cache_page(lower_inode->i_mapping, page->index);
	if (!lower_page)
		goto out;

	/* do the actual sync */
	mapping = lower_page->mapping;
	/*
	 * XXX: can we optimize ala RAIF and set the lower page to be
	 * discarded after a successful sync_page?
	 */
	if (mapping && mapping->a_ops && mapping->a_ops->sync_page)
		mapping->a_ops->sync_page(lower_page);

	unlock_page(lower_page);	/* b/c grab_cache_page locked it */
	/* b/c grab_cache_page increased refcnt */
	page_cache_release(lower_page);

out:
	return;
}

struct address_space_operations unionfs_aops = {
	.writepage	= unionfs_writepage,
	.readpage	= unionfs_readpage,
	.prepare_write	= unionfs_prepare_write,
	.commit_write	= unionfs_commit_write,
	.sync_page	= unionfs_sync_page,
};
