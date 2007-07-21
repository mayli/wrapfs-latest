/*
 * Copyright (c) 2003-2007 Erez Zadok
 * Copyright (c) 2003-2006 Charles P. Wright
 * Copyright (c) 2005-2007 Josef 'Jeff' Sipek
 * Copyright (c) 2005-2006 Junjiro Okajima
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
 * 1) Copyup the file
 * 2) Rename the file to '.unionfs<original inode#><counter>' - obviously
 * stolen from NFS's silly rename
 */
static int copyup_deleted_file(struct file *file, struct dentry *dentry,
			       int bstart, int bindex)
{
	static unsigned int counter;
	const int i_inosize = sizeof(dentry->d_inode->i_ino) * 2;
	const int countersize = sizeof(counter) * 2;
	const int nlen = sizeof(".unionfs") + i_inosize + countersize - 1;
	char name[nlen + 1];
	int err;
	struct dentry *tmp_dentry = NULL;
	struct dentry *lower_dentry;
	struct dentry *lower_dir_dentry = NULL;

	lower_dentry = unionfs_lower_dentry_idx(dentry, bstart);

	sprintf(name, ".unionfs%*.*lx",
		i_inosize, i_inosize, lower_dentry->d_inode->i_ino);

	/*
	 * Loop, looking for an unused temp name to copyup to.
	 *
	 * It's somewhat silly that we look for a free temp tmp name in the
	 * source branch (bstart) instead of the dest branch (bindex), where
	 * the final name will be created.  We _will_ catch it if somehow
	 * the name exists in the dest branch, but it'd be nice to catch it
	 * sooner than later.
	 */
retry:
	tmp_dentry = NULL;
	do {
		char *suffix = name + nlen - countersize;

		dput(tmp_dentry);
		counter++;
		sprintf(suffix, "%*.*x", countersize, countersize, counter);

		printk(KERN_DEBUG "unionfs: trying to rename %s to %s\n",
		       dentry->d_name.name, name);

		tmp_dentry = lookup_one_len(name, lower_dentry->d_parent,
					    nlen);
		if (IS_ERR(tmp_dentry)) {
			err = PTR_ERR(tmp_dentry);
			goto out;
		}
		/* don't dput here because of do-while condition eval order */
	} while (tmp_dentry->d_inode != NULL);	/* need negative dentry */
	dput(tmp_dentry);

	err = copyup_named_file(dentry->d_parent->d_inode, file, name, bstart,
				bindex, file->f_path.dentry->d_inode->i_size);
	if (err) {
		if (err == -EEXIST)
			goto retry;
		goto out;
	}

	/* bring it to the same state as an unlinked file */
	lower_dentry = unionfs_lower_dentry_idx(dentry, dbstart(dentry));
	if (!unionfs_lower_inode_idx(dentry->d_inode, bindex)) {
		atomic_inc(&lower_dentry->d_inode->i_count);
		unionfs_set_lower_inode_idx(dentry->d_inode, bindex,
					    lower_dentry->d_inode);
	}
	lower_dir_dentry = lock_parent(lower_dentry);
	err = vfs_unlink(lower_dir_dentry->d_inode, lower_dentry);
	unlock_dir(lower_dir_dentry);

out:
	if (!err)
		unionfs_check_dentry(dentry);
	return err;
}

/*
 * put all references held by upper struct file and free lower file pointer
 * array
 */
static void cleanup_file(struct file *file)
{
	int bindex, bstart, bend;
	struct file **lf;
	struct super_block *sb = file->f_path.dentry->d_sb;

	lf = UNIONFS_F(file)->lower_files;
	bstart = fbstart(file);
	bend = fbend(file);

	for (bindex = bstart; bindex <= bend; bindex++) {
		if (unionfs_lower_file_idx(file, bindex)) {
			/*
			 * Find new index of matching branch with an open
			 * file, since branches could have been added or
			 * deleted causing the one with open files to shift.
			 */
			int i;	/* holds (possibly) updated branch index */
			int old_bid;

			old_bid = UNIONFS_F(file)->saved_branch_ids[bindex];
			i = branch_id_to_idx(sb, old_bid);
			if (i < 0)
				printk(KERN_ERR "unionfs: no superblock for "
				       "file %p\n", file);
			else {
				/* decrement count of open files */
				branchput(sb, i);
				/*
				 * fput will perform an mntput for us on the
				 * correct branch.  Although we're using the
				 * file's old branch configuration, bindex,
				 * which is the old index, correctly points
				 * to the right branch in the file's branch
				 * list.  In other words, we're going to
				 * mntput the correct branch even if
				 * branches have been added/removed.
				 */
				fput(unionfs_lower_file_idx(file, bindex));
			}
		}
	}

	UNIONFS_F(file)->lower_files = NULL;
	kfree(lf);
	kfree(UNIONFS_F(file)->saved_branch_ids);
	/* set to NULL because caller needs to know if to kfree on error */
	UNIONFS_F(file)->saved_branch_ids = NULL;
}

/* open all lower files for a given file */
static int open_all_files(struct file *file)
{
	int bindex, bstart, bend, err = 0;
	struct file *lower_file;
	struct dentry *lower_dentry;
	struct dentry *dentry = file->f_path.dentry;
	struct super_block *sb = dentry->d_sb;

	bstart = dbstart(dentry);
	bend = dbend(dentry);

	for (bindex = bstart; bindex <= bend; bindex++) {
		lower_dentry = unionfs_lower_dentry_idx(dentry, bindex);
		if (!lower_dentry)
			continue;

		dget(lower_dentry);
		unionfs_mntget(dentry, bindex);
		branchget(sb, bindex);

		lower_file =
			dentry_open(lower_dentry,
				    unionfs_lower_mnt_idx(dentry, bindex),
				    file->f_flags);
		if (IS_ERR(lower_file)) {
			err = PTR_ERR(lower_file);
			goto out;
		} else
			unionfs_set_lower_file_idx(file, bindex, lower_file);
	}
out:
	return err;
}

/* open the highest priority file for a given upper file */
static int open_highest_file(struct file *file, int willwrite)
{
	int bindex, bstart, bend, err = 0;
	struct file *lower_file;
	struct dentry *lower_dentry;
	struct dentry *dentry = file->f_path.dentry;
	struct inode *parent_inode = dentry->d_parent->d_inode;
	struct super_block *sb = dentry->d_sb;
	size_t inode_size = dentry->d_inode->i_size;

	bstart = dbstart(dentry);
	bend = dbend(dentry);

	lower_dentry = unionfs_lower_dentry(dentry);
	if (willwrite && IS_WRITE_FLAG(file->f_flags) && is_robranch(dentry)) {
		for (bindex = bstart - 1; bindex >= 0; bindex--) {
			err = copyup_file(parent_inode, file, bstart, bindex,
					  inode_size);
			if (!err)
				break;
		}
		atomic_set(&UNIONFS_F(file)->generation,
			   atomic_read(&UNIONFS_I(dentry->d_inode)->
				       generation));
		goto out;
	}

	dget(lower_dentry);
	unionfs_mntget(dentry, bstart);
	lower_file = dentry_open(lower_dentry,
				 unionfs_lower_mnt_idx(dentry, bstart),
				 file->f_flags);
	if (IS_ERR(lower_file)) {
		err = PTR_ERR(lower_file);
		goto out;
	}
	branchget(sb, bstart);
	unionfs_set_lower_file(file, lower_file);
	/* Fix up the position. */
	lower_file->f_pos = file->f_pos;

	memcpy(&lower_file->f_ra, &file->f_ra, sizeof(struct file_ra_state));
out:
	return err;
}

/* perform a delayed copyup of a read-write file on a read-only branch */
static int do_delayed_copyup(struct file *file)
{
	int bindex, bstart, bend, err = 0;
	struct dentry *dentry = file->f_path.dentry;
	struct inode *parent_inode = dentry->d_parent->d_inode;
	loff_t inode_size = dentry->d_inode->i_size;

	bstart = fbstart(file);
	bend = fbend(file);

	BUG_ON(!S_ISREG(dentry->d_inode->i_mode));

	unionfs_check_file(file);
	unionfs_check_dentry(dentry);
	for (bindex = bstart - 1; bindex >= 0; bindex--) {
		if (!d_deleted(dentry))
			err = copyup_file(parent_inode, file, bstart,
					  bindex, inode_size);
		else
			err = copyup_deleted_file(file, dentry, bstart,
						  bindex);

		if (!err)
			break;
	}
	if (err || (bstart <= fbstart(file)))
		goto out;
	bend = fbend(file);
	for (bindex = bstart; bindex <= bend; bindex++) {
		if (unionfs_lower_file_idx(file, bindex)) {
			branchput(dentry->d_sb, bindex);
			fput(unionfs_lower_file_idx(file, bindex));
			unionfs_set_lower_file_idx(file, bindex, NULL);
		}
		if (unionfs_lower_mnt_idx(dentry, bindex)) {
			unionfs_mntput(dentry, bindex);
			unionfs_set_lower_mnt_idx(dentry, bindex, NULL);
		}
		if (unionfs_lower_dentry_idx(dentry, bindex)) {
			BUG_ON(!dentry->d_inode);
			iput(unionfs_lower_inode_idx(dentry->d_inode, bindex));
			unionfs_set_lower_inode_idx(dentry->d_inode, bindex,
						    NULL);
			dput(unionfs_lower_dentry_idx(dentry, bindex));
			unionfs_set_lower_dentry_idx(dentry, bindex, NULL);
		}
	}
	/* for reg file, we only open it "once" */
	fbend(file) = fbstart(file);
	set_dbend(dentry, dbstart(dentry));
	ibend(dentry->d_inode) = ibstart(dentry->d_inode);

out:
	unionfs_check_file(file);
	unionfs_check_dentry(dentry);
	return err;
}

/*
 * Revalidate the struct file
 * @file: file to revalidate
 * @willwrite: 1 if caller may cause changes to the file; 0 otherwise.
 */
int unionfs_file_revalidate(struct file *file, int willwrite)
{
	struct super_block *sb;
	struct dentry *dentry;
	int sbgen, fgen, dgen;
	int bstart, bend;
	int size;
	int err = 0;

	dentry = file->f_path.dentry;
	unionfs_lock_dentry(dentry);
	sb = dentry->d_sb;

	/*
	 * First revalidate the dentry inside struct file,
	 * but not unhashed dentries.
	 */
	if (!d_deleted(dentry) &&
	    !__unionfs_d_revalidate_chain(dentry, NULL, willwrite)) {
		err = -ESTALE;
		goto out_nofree;
	}

	sbgen = atomic_read(&UNIONFS_SB(sb)->generation);
	dgen = atomic_read(&UNIONFS_D(dentry)->generation);
	fgen = atomic_read(&UNIONFS_F(file)->generation);

	BUG_ON(sbgen > dgen);

	/*
	 * There are two cases we are interested in.  The first is if the
	 * generation is lower than the super-block.  The second is if
	 * someone has copied up this file from underneath us, we also need
	 * to refresh things.
	 */
	if (!d_deleted(dentry) &&
	    (sbgen > fgen || dbstart(dentry) != fbstart(file))) {
		int orig_brid =	/* save orig branch ID */
			UNIONFS_F(file)->saved_branch_ids[fbstart(file)];

		/* First we throw out the existing files. */
		cleanup_file(file);

		/* Now we reopen the file(s) as in unionfs_open. */
		bstart = fbstart(file) = dbstart(dentry);
		bend = fbend(file) = dbend(dentry);

		size = sizeof(struct file *) * sbmax(sb);
		UNIONFS_F(file)->lower_files = kzalloc(size, GFP_KERNEL);
		if (!UNIONFS_F(file)->lower_files) {
			err = -ENOMEM;
			goto out;
		}
		size = sizeof(int) * sbmax(sb);
		UNIONFS_F(file)->saved_branch_ids = kzalloc(size, GFP_KERNEL);
		if (!UNIONFS_F(file)->saved_branch_ids) {
			err = -ENOMEM;
			goto out;
		}

		if (S_ISDIR(dentry->d_inode->i_mode)) {
			/* We need to open all the files. */
			err = open_all_files(file);
			if (err)
				goto out;
		} else {
			int new_brid;
			/* We only open the highest priority branch. */
			err = open_highest_file(file, willwrite);
			if (err)
				goto out;
			new_brid = UNIONFS_F(file)->
			  saved_branch_ids[fbstart(file)];
			if (new_brid != orig_brid && sbgen > fgen) {
				/*
				 * If we re-opened the file on a different
				 * branch than the original one, and this
				 * was due to a new branch inserted, then
				 * update the mnt counts of the old and new
				 * branches accordingly.
				 */
				unionfs_mntget(dentry, bstart);	/* new branch */
				unionfs_mntput(sb->s_root, /* orig branch */
					       branch_id_to_idx(sb, orig_brid));
			}
		}
		atomic_set(&UNIONFS_F(file)->generation,
			   atomic_read(&UNIONFS_I(dentry->d_inode)->
				       generation));
	}

	/* Copyup on the first write to a file on a readonly branch. */
	if (willwrite && IS_WRITE_FLAG(file->f_flags) &&
	    !IS_WRITE_FLAG(unionfs_lower_file(file)->f_flags) &&
	    is_robranch(dentry)) {
		printk(KERN_DEBUG "unionfs: doing delayed copyup of a "
		       "read-write file on a read-only branch\n");
		err = do_delayed_copyup(file);
	}

out:
	if (err) {
		kfree(UNIONFS_F(file)->lower_files);
		kfree(UNIONFS_F(file)->saved_branch_ids);
	}
out_nofree:
	if (!err)
		unionfs_check_file(file);
	unionfs_unlock_dentry(dentry);
	return err;
}

/* unionfs_open helper function: open a directory */
static int __open_dir(struct inode *inode, struct file *file)
{
	struct dentry *lower_dentry;
	struct file *lower_file;
	int bindex, bstart, bend;

	bstart = fbstart(file) = dbstart(file->f_path.dentry);
	bend = fbend(file) = dbend(file->f_path.dentry);

	for (bindex = bstart; bindex <= bend; bindex++) {
		lower_dentry =
			unionfs_lower_dentry_idx(file->f_path.dentry, bindex);
		if (!lower_dentry)
			continue;

		dget(lower_dentry);
		unionfs_mntget(file->f_path.dentry, bindex);
		lower_file = dentry_open(lower_dentry,
					 unionfs_lower_mnt_idx(file->f_path.dentry,
							       bindex),
					 file->f_flags);
		if (IS_ERR(lower_file))
			return PTR_ERR(lower_file);

		unionfs_set_lower_file_idx(file, bindex, lower_file);

		/*
		 * The branchget goes after the open, because otherwise
		 * we would miss the reference on release.
		 */
		branchget(inode->i_sb, bindex);
	}

	return 0;
}

/* unionfs_open helper function: open a file */
static int __open_file(struct inode *inode, struct file *file)
{
	struct dentry *lower_dentry;
	struct file *lower_file;
	int lower_flags;
	int bindex, bstart, bend;

	lower_dentry = unionfs_lower_dentry(file->f_path.dentry);
	lower_flags = file->f_flags;

	bstart = fbstart(file) = dbstart(file->f_path.dentry);
	bend = fbend(file) = dbend(file->f_path.dentry);

	/*
	 * check for the permission for lower file.  If the error is
	 * COPYUP_ERR, copyup the file.
	 */
	if (lower_dentry->d_inode && is_robranch(file->f_path.dentry)) {
		/*
		 * if the open will change the file, copy it up otherwise
		 * defer it.
		 */
		if (lower_flags & O_TRUNC) {
			int size = 0;
			int err = -EROFS;

			/* copyup the file */
			for (bindex = bstart - 1; bindex >= 0; bindex--) {
				err = copyup_file(
					file->f_path.dentry->d_parent->d_inode,
					file, bstart, bindex, size);
				if (!err)
					break;
			}
			return err;
		} else
			lower_flags &= ~(OPEN_WRITE_FLAGS);
	}

	dget(lower_dentry);

	/*
	 * dentry_open will decrement mnt refcnt if err.
	 * otherwise fput() will do an mntput() for us upon file close.
	 */
	unionfs_mntget(file->f_path.dentry, bstart);
	lower_file =
		dentry_open(lower_dentry,
			    unionfs_lower_mnt_idx(file->f_path.dentry, bstart),
			    lower_flags);
	if (IS_ERR(lower_file))
		return PTR_ERR(lower_file);

	unionfs_set_lower_file(file, lower_file);
	branchget(inode->i_sb, bstart);

	return 0;
}

int unionfs_open(struct inode *inode, struct file *file)
{
	int err = 0;
	struct file *lower_file = NULL;
	struct dentry *dentry = NULL;
	int bindex = 0, bstart = 0, bend = 0;
	int size;

	unionfs_read_lock(inode->i_sb);

	file->private_data =
		kzalloc(sizeof(struct unionfs_file_info), GFP_KERNEL);
	if (!UNIONFS_F(file)) {
		err = -ENOMEM;
		goto out_nofree;
	}
	fbstart(file) = -1;
	fbend(file) = -1;
	atomic_set(&UNIONFS_F(file)->generation,
		   atomic_read(&UNIONFS_I(inode)->generation));

	size = sizeof(struct file *) * sbmax(inode->i_sb);
	UNIONFS_F(file)->lower_files = kzalloc(size, GFP_KERNEL);
	if (!UNIONFS_F(file)->lower_files) {
		err = -ENOMEM;
		goto out;
	}
	size = sizeof(int) * sbmax(inode->i_sb);
	UNIONFS_F(file)->saved_branch_ids = kzalloc(size, GFP_KERNEL);
	if (!UNIONFS_F(file)->saved_branch_ids) {
		err = -ENOMEM;
		goto out;
	}

	dentry = file->f_path.dentry;
	unionfs_lock_dentry(dentry);

	bstart = fbstart(file) = dbstart(dentry);
	bend = fbend(file) = dbend(dentry);

	/* increment, so that we can flush appropriately */
	atomic_inc(&UNIONFS_I(dentry->d_inode)->totalopens);

	/*
	 * open all directories and make the unionfs file struct point to
	 * these lower file structs
	 */
	if (S_ISDIR(inode->i_mode))
		err = __open_dir(inode, file);	/* open a dir */
	else
		err = __open_file(inode, file);	/* open a file */

	/* freeing the allocated resources, and fput the opened files */
	if (err) {
		atomic_dec(&UNIONFS_I(dentry->d_inode)->totalopens);
		for (bindex = bstart; bindex <= bend; bindex++) {
			lower_file = unionfs_lower_file_idx(file, bindex);
			if (!lower_file)
				continue;

			branchput(file->f_path.dentry->d_sb, bindex);
			/* fput calls dput for lower_dentry */
			fput(lower_file);
		}
	}

	unionfs_unlock_dentry(dentry);

out:
	if (err) {
		kfree(UNIONFS_F(file)->lower_files);
		kfree(UNIONFS_F(file)->saved_branch_ids);
		kfree(UNIONFS_F(file));
	}
out_nofree:
	unionfs_read_unlock(inode->i_sb);
	unionfs_check_inode(inode);
	if (!err) {
		unionfs_check_file(file);
		unionfs_check_dentry(file->f_path.dentry->d_parent);
	}
	return err;
}

/*
 * release all lower object references & free the file info structure
 *
 * No need to grab sb info's rwsem.
 */
int unionfs_file_release(struct inode *inode, struct file *file)
{
	struct file *lower_file = NULL;
	struct unionfs_file_info *fileinfo;
	struct unionfs_inode_info *inodeinfo;
	struct super_block *sb = inode->i_sb;
	int bindex, bstart, bend;
	int fgen, err = 0;

	unionfs_read_lock(sb);
	/*
	 * Yes, we have to revalidate this file even if it's being released.
	 * This is important for open-but-unlinked files, as well as mmap
	 * support.
	 */
	if ((err = unionfs_file_revalidate(file, 1)))
		goto out;
	unionfs_check_file(file);
	fileinfo = UNIONFS_F(file);
	BUG_ON(file->f_path.dentry->d_inode != inode);
	inodeinfo = UNIONFS_I(inode);

	/* fput all the lower files */
	fgen = atomic_read(&fileinfo->generation);
	bstart = fbstart(file);
	bend = fbend(file);

	for (bindex = bstart; bindex <= bend; bindex++) {
		lower_file = unionfs_lower_file_idx(file, bindex);

		if (lower_file) {
			fput(lower_file);
			branchput(sb, bindex);
		}
	}
	kfree(fileinfo->lower_files);
	kfree(fileinfo->saved_branch_ids);

	if (fileinfo->rdstate) {
		fileinfo->rdstate->access = jiffies;
		printk(KERN_DEBUG "unionfs: saving rdstate with cookie "
		       "%u [%d.%lld]\n",
		       fileinfo->rdstate->cookie,
		       fileinfo->rdstate->bindex,
		       (long long)fileinfo->rdstate->dirpos);
		spin_lock(&inodeinfo->rdlock);
		inodeinfo->rdcount++;
		list_add_tail(&fileinfo->rdstate->cache,
			      &inodeinfo->readdircache);
		mark_inode_dirty(inode);
		spin_unlock(&inodeinfo->rdlock);
		fileinfo->rdstate = NULL;
	}
	kfree(fileinfo);

out:
	unionfs_read_unlock(sb);
	return err;
}

/* pass the ioctl to the lower fs */
static long do_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct file *lower_file;
	int err;

	lower_file = unionfs_lower_file(file);

	err = security_file_ioctl(lower_file, cmd, arg);
	if (err)
		goto out;

	err = -ENOTTY;
	if (!lower_file || !lower_file->f_op)
		goto out;
	if (lower_file->f_op->unlocked_ioctl) {
		err = lower_file->f_op->unlocked_ioctl(lower_file, cmd, arg);
	} else if (lower_file->f_op->ioctl) {
		lock_kernel();
		err = lower_file->f_op->ioctl(lower_file->f_path.dentry->d_inode,
					      lower_file, cmd, arg);
		unlock_kernel();
	}

out:
	return err;
}

/*
 * return to user-space the branch indices containing the file in question
 *
 * We use fd_set and therefore we are limited to the number of the branches
 * to FD_SETSIZE, which is currently 1024 - plenty for most people
 */
static int unionfs_ioctl_queryfile(struct file *file, unsigned int cmd,
				   unsigned long arg)
{
	int err = 0;
	fd_set branchlist;
	int bstart = 0, bend = 0, bindex = 0;
	int orig_bstart, orig_bend;
	struct dentry *dentry, *lower_dentry;
	struct vfsmount *mnt;

	dentry = file->f_path.dentry;
	unionfs_lock_dentry(dentry);
	orig_bstart = dbstart(dentry);
	orig_bend = dbend(dentry);
	if ((err = unionfs_partial_lookup(dentry)))
		goto out;
	bstart = dbstart(dentry);
	bend = dbend(dentry);

	FD_ZERO(&branchlist);

	for (bindex = bstart; bindex <= bend; bindex++) {
		lower_dentry = unionfs_lower_dentry_idx(dentry, bindex);
		if (!lower_dentry)
			continue;
		if (lower_dentry->d_inode)
			FD_SET(bindex, &branchlist);
		/* purge any lower objects after partial_lookup */
		if (bindex < orig_bstart || bindex > orig_bend) {
			dput(lower_dentry);
			unionfs_set_lower_dentry_idx(dentry, bindex, NULL);
			iput(unionfs_lower_inode_idx(dentry->d_inode, bindex));
			unionfs_set_lower_inode_idx(dentry->d_inode, bindex,
						    NULL);
			mnt = unionfs_lower_mnt_idx(dentry, bindex);
			if (!mnt)
				continue;
			unionfs_mntput(dentry, bindex);
			unionfs_set_lower_mnt_idx(dentry, bindex, NULL);
		}
	}
	/* restore original dentry's offsets */
	set_dbstart(dentry, orig_bstart);
	set_dbend(dentry, orig_bend);
	ibstart(dentry->d_inode) = orig_bstart;
	ibend(dentry->d_inode) = orig_bend;

	err = copy_to_user((void __user *)arg, &branchlist, sizeof(fd_set));
	if (err)
		err = -EFAULT;

out:
	unionfs_unlock_dentry(dentry);
	return err < 0 ? err : bend;
}

long unionfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long err;

	unionfs_read_lock(file->f_path.dentry->d_sb);

	if ((err = unionfs_file_revalidate(file, 1)))
		goto out;

	/* check if asked for local commands */
	switch (cmd) {
	case UNIONFS_IOCTL_INCGEN:
		/* Increment the superblock generation count */
		printk("unionfs: incgen ioctl deprecated; "
		       "use \"-o remount,incgen\"\n");
		err = -ENOSYS;
		break;

	case UNIONFS_IOCTL_QUERYFILE:
		/* Return list of branches containing the given file */
		err = unionfs_ioctl_queryfile(file, cmd, arg);
		break;

	default:
		/* pass the ioctl down */
		err = do_ioctl(file, cmd, arg);
		break;
	}

out:
	unionfs_read_unlock(file->f_path.dentry->d_sb);
	unionfs_check_file(file);
	return err;
}

int unionfs_flush(struct file *file, fl_owner_t id)
{
	int err = 0;
	struct file *lower_file = NULL;
	struct dentry *dentry = file->f_path.dentry;
	int bindex, bstart, bend;

	unionfs_read_lock(dentry->d_sb);

	if ((err = unionfs_file_revalidate(file, 1)))
		goto out;
	unionfs_check_file(file);

	if (!atomic_dec_and_test(&UNIONFS_I(dentry->d_inode)->totalopens))
		goto out;

	unionfs_lock_dentry(dentry);

	bstart = fbstart(file);
	bend = fbend(file);
	for (bindex = bstart; bindex <= bend; bindex++) {
		lower_file = unionfs_lower_file_idx(file, bindex);

		if (lower_file && lower_file->f_op &&
		    lower_file->f_op->flush) {
			err = lower_file->f_op->flush(lower_file, id);
			if (err)
				goto out_lock;

			/* if there are no more refs to the dentry, dput it */
			if (d_deleted(dentry)) {
				dput(unionfs_lower_dentry_idx(dentry, bindex));
				unionfs_set_lower_dentry_idx(dentry, bindex,
							     NULL);
			}
		}

	}

	/* on success, update our times */
	unionfs_copy_attr_times(dentry->d_inode);
	/* parent time could have changed too (async) */
	unionfs_copy_attr_times(dentry->d_parent->d_inode);

out_lock:
	unionfs_unlock_dentry(dentry);
out:
	unionfs_read_unlock(dentry->d_sb);
	unionfs_check_file(file);
	return err;
}
