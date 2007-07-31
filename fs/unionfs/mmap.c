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
static int unionfs_writepage(struct page *page, struct writeback_control *wbc)
{
	int err = -EIO;
	struct inode *inode;
	struct inode *lower_inode;
	struct page *lower_page;
	char *kaddr, *lower_kaddr;
	int saved_for_writepages = wbc->for_writepages;

	inode = page->mapping->host;
	lower_inode = unionfs_lower_inode(inode);

	/*
	 * find lower page (returns a locked page)
	 *
	 * NOTE: we used to call grab_cache_page(), but that was unnecessary
	 * as it would have tried to create a new lower page if it didn't
	 * exist, leading to deadlocks (esp. under memory-pressure
	 * conditions, when it is really a bad idea to *consume* more
	 * memory).  Instead, we assume the lower page exists, and if we can
	 * find it, then we ->writepage on it; if we can't find it, then it
	 * couldn't have disappeared unless the kernel already flushed it,
	 * in which case we're still OK.  This is especially correct if
	 * wbc->sync_mode is WB_SYNC_NONE (as per
	 * Documentation/filesystems/vfs.txt).  If we can't flush our page
	 * because we can't find a lower page, then at least we re-mark our
	 * page as dirty, and return AOP_WRITEPAGE_ACTIVATE as the VFS
	 * expects us to.  (Note, if in the future it'd turn out that we
	 * have to find a lower page no matter what, then we'd have to
	 * resort to RAIF's page pointer flipping trick.)
	 */
	lower_page = find_lock_page(lower_inode->i_mapping, page->index);
	if (!lower_page) {
		err = AOP_WRITEPAGE_ACTIVATE;
		set_page_dirty(page);
		goto out;
	}

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
	clear_page_dirty_for_io(lower_page); /* emulate VFS behavior */
	err = lower_inode->i_mapping->a_ops->writepage(lower_page, wbc);
	wbc->for_writepages = saved_for_writepages; /* restore value */

	/* b/c find_lock_page locked it and ->writepage unlocks on success */
	if (err)
		unlock_page(lower_page);
	/* b/c grab_cache_page increased refcnt */
	page_cache_release(lower_page);

	if (err < 0) {
		ClearPageUptodate(page);
		goto out;
	}
	if (err == AOP_WRITEPAGE_ACTIVATE) {
		/*
		 * Lower file systems such as ramfs and tmpfs, may return
		 * AOP_WRITEPAGE_ACTIVATE so that the VM won't try to
		 * (pointlessly) write the page again for a while.  But
		 * those lower file systems also set the page dirty bit back
		 * again.  So we mimic that behaviour here.
		 */
		if (PageDirty(lower_page))
			set_page_dirty(page);
		goto out;
	}

	/* all is well */
	SetPageUptodate(page);
	/* lower mtimes has changed: update ours */
	unionfs_copy_attr_times(inode);

	unlock_page(page);

out:
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
	struct file *lower_file;
	struct inode *inode;
	mm_segment_t old_fs;
	char *page_data = NULL;
	loff_t offset;

	if (UNIONFS_F(file) == NULL) {
		err = -ENOENT;
		goto out;
	}

	lower_file = unionfs_lower_file(file);
	/* FIXME: is this assertion right here? */
	BUG_ON(lower_file == NULL);

	inode = file->f_path.dentry->d_inode;

	page_data = (char *) kmap(page);
	/*
	 * Use vfs_read because some lower file systems don't have a
	 * readpage method, and some file systems (esp. distributed ones)
	 * don't like their pages to be accessed directly.  Using vfs_read
	 * may be a little slower, but a lot safer, as the VFS does a lot of
	 * the necessary magic for us.
	 */
	offset = lower_file->f_pos = (page->index << PAGE_CACHE_SHIFT);
	old_fs = get_fs();
	set_fs(KERNEL_DS);
	err = vfs_read(lower_file, page_data, PAGE_CACHE_SIZE,
		       &lower_file->f_pos);
	set_fs(old_fs);

	kunmap(page);

	if (err < 0)
		goto out;
	err = 0;

	/* if vfs_read succeeded above, sync up our times */
	unionfs_copy_attr_times(inode);

	flush_dcache_page(page);

out:
	if (err == 0)
		SetPageUptodate(page);
	else
		ClearPageUptodate(page);

	return err;
}

static int unionfs_readpage(struct file *file, struct page *page)
{
	int err;

	unionfs_read_lock(file->f_path.dentry->d_sb);
	if ((err = unionfs_file_revalidate(file, 0)))
		goto out;
	unionfs_check_file(file);

	err = unionfs_do_readpage(file, page);

	if (!err) {
		touch_atime(unionfs_lower_mnt(file->f_path.dentry),
			    unionfs_lower_dentry(file->f_path.dentry));
		unionfs_copy_attr_times(file->f_path.dentry->d_inode);
	}

	/*
	 * we have to unlock our page, b/c we _might_ have gotten a locked
	 * page.  but we no longer have to wakeup on our page here, b/c
	 * UnlockPage does it
	 */
out:
	unlock_page(page);
	unionfs_check_file(file);
	unionfs_read_unlock(file->f_path.dentry->d_sb);

	return err;
}

static int unionfs_prepare_write(struct file *file, struct page *page,
				 unsigned from, unsigned to)
{
	int err;

	unionfs_read_lock(file->f_path.dentry->d_sb);
	/*
	 * This is the only place where we unconditionally copy the lower
	 * attribute times before calling unionfs_file_revalidate.  The
	 * reason is that our ->write calls do_sync_write which in turn will
	 * call our ->prepare_write and then ->commit_write.  Before our
	 * ->write is called, the lower mtimes are in sync, but by the time
	 * the VFS calls our ->commit_write, the lower mtimes have changed.
	 * Therefore, the only reasonable time for us to sync up from the
	 * changed lower mtimes, and avoid an invariant violation warning,
	 * is here, in ->prepare_write.
	 */
	unionfs_copy_attr_times(file->f_path.dentry->d_inode);
	err = unionfs_file_revalidate(file, 1);
	unionfs_check_file(file);
	unionfs_read_unlock(file->f_path.dentry->d_sb);

	return err;
}

static int unionfs_commit_write(struct file *file, struct page *page,
				unsigned from, unsigned to)
{
	int err = -ENOMEM;
	struct inode *inode, *lower_inode;
	struct file *lower_file = NULL;
	loff_t pos;
	unsigned bytes = to - from;
	char *page_data = NULL;
	mm_segment_t old_fs;

	BUG_ON(file == NULL);

	unionfs_read_lock(file->f_path.dentry->d_sb);
	if ((err = unionfs_file_revalidate(file, 1)))
		goto out;
	unionfs_check_file(file);

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
	/* if vfs_write succeeded above, sync up our times */
	unionfs_copy_attr_times(inode);
	mark_inode_dirty_sync(inode);

out:
	if (err < 0)
		ClearPageUptodate(page);

	unionfs_read_unlock(file->f_path.dentry->d_sb);
	unionfs_check_file(file);
	return err;		/* assume all is ok */
}

static void unionfs_sync_page(struct page *page)
{
	struct inode *inode;
	struct inode *lower_inode;
	struct page *lower_page;
	struct address_space *mapping;

	inode = page->mapping->host;
	lower_inode = unionfs_lower_inode(inode);

	/*
	 * Find lower page (returns a locked page).
	 *
	 * NOTE: we used to call grab_cache_page(), but that was unnecessary
	 * as it would have tried to create a new lower page if it didn't
	 * exist, leading to deadlocks.  All our sync_page method needs to
	 * do is ensure that pending I/O gets done.
	 */
	lower_page = find_lock_page(lower_inode->i_mapping, page->index);
	if (!lower_page) {
		printk(KERN_DEBUG "unionfs: find_lock_page failed\n");
		goto out;
	}

	/* do the actual sync */
	mapping = lower_page->mapping;
	/*
	 * XXX: can we optimize ala RAIF and set the lower page to be
	 * discarded after a successful sync_page?
	 */
	if (mapping && mapping->a_ops && mapping->a_ops->sync_page)
		mapping->a_ops->sync_page(lower_page);

	/* b/c find_lock_page locked it */
	unlock_page(lower_page);
	/* b/c find_lock_page increased refcnt */
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
