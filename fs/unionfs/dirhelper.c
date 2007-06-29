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
 * Delete all of the whiteouts in a given directory for rmdir.
 *
 * lower directory inode should be locked
 */
int do_delete_whiteouts(struct dentry *dentry, int bindex,
			struct unionfs_dir_state *namelist)
{
	int err = 0;
	struct dentry *lower_dir_dentry = NULL;
	struct dentry *lower_dentry;
	char *name = NULL, *p;
	struct inode *lower_dir;
	int i;
	struct list_head *pos;
	struct filldir_node *cursor;

	/* Find out lower parent dentry */
	lower_dir_dentry = unionfs_lower_dentry_idx(dentry, bindex);
	BUG_ON(!S_ISDIR(lower_dir_dentry->d_inode->i_mode));
	lower_dir = lower_dir_dentry->d_inode;
	BUG_ON(!S_ISDIR(lower_dir->i_mode));

	err = -ENOMEM;
	name = __getname();
	if (!name)
		goto out;
	strcpy(name, UNIONFS_WHPFX);
	p = name + UNIONFS_WHLEN;

	err = 0;
	for (i = 0; !err && i < namelist->size; i++) {
		list_for_each(pos, &namelist->list[i]) {
			cursor =
				list_entry(pos, struct filldir_node,
					   file_list);
			/* Only operate on whiteouts in this branch. */
			if (cursor->bindex != bindex)
				continue;
			if (!cursor->whiteout)
				continue;

			strcpy(p, cursor->name);
			lower_dentry =
				lookup_one_len(name, lower_dir_dentry,
					       cursor->namelen +
					       UNIONFS_WHLEN);
			if (IS_ERR(lower_dentry)) {
				err = PTR_ERR(lower_dentry);
				break;
			}
			if (lower_dentry->d_inode)
				err = vfs_unlink(lower_dir, lower_dentry);
			dput(lower_dentry);
			if (err)
				break;
		}
	}

	__putname(name);

	/* After all of the removals, we should copy the attributes once. */
	fsstack_copy_attr_times(dentry->d_inode, lower_dir_dentry->d_inode);

out:
	return err;
}

/* delete whiteouts in a dir (for rmdir operation) using sioq if necessary */
int delete_whiteouts(struct dentry *dentry, int bindex,
		     struct unionfs_dir_state *namelist)
{
	int err;
	struct super_block *sb;
	struct dentry *lower_dir_dentry;
	struct inode *lower_dir;
	struct sioq_args args;

	sb = dentry->d_sb;

	BUG_ON(!S_ISDIR(dentry->d_inode->i_mode));
	BUG_ON(bindex < dbstart(dentry));
	BUG_ON(bindex > dbend(dentry));
	err = is_robranch_super(sb, bindex);
	if (err)
		goto out;

	lower_dir_dentry = unionfs_lower_dentry_idx(dentry, bindex);
	BUG_ON(!S_ISDIR(lower_dir_dentry->d_inode->i_mode));
	lower_dir = lower_dir_dentry->d_inode;
	BUG_ON(!S_ISDIR(lower_dir->i_mode));

	mutex_lock(&lower_dir->i_mutex);
	if (!permission(lower_dir, MAY_WRITE | MAY_EXEC, NULL))
		err = do_delete_whiteouts(dentry, bindex, namelist);
	else {
		args.deletewh.namelist = namelist;
		args.deletewh.dentry = dentry;
		args.deletewh.bindex = bindex;
		run_sioq(__delete_whiteouts, &args);
		err = args.err;
	}
	mutex_unlock(&lower_dir->i_mutex);

out:
	return err;
}

#define RD_NONE 0
#define RD_CHECK_EMPTY 1
/* The callback structure for check_empty. */
struct unionfs_rdutil_callback {
	int err;
	int filldir_called;
	struct unionfs_dir_state *rdstate;
	int mode;
};

/* This filldir function makes sure only whiteouts exist within a directory. */
static int readdir_util_callback(void *dirent, const char *name, int namelen,
				 loff_t offset, u64 ino, unsigned int d_type)
{
	int err = 0;
	struct unionfs_rdutil_callback *buf = dirent;
	int whiteout = 0;
	struct filldir_node *found;

	buf->filldir_called = 1;

	if (name[0] == '.' && (namelen == 1 ||
			       (name[1] == '.' && namelen == 2)))
		goto out;

	if (namelen > UNIONFS_WHLEN &&
	    !strncmp(name, UNIONFS_WHPFX, UNIONFS_WHLEN)) {
		namelen -= UNIONFS_WHLEN;
		name += UNIONFS_WHLEN;
		whiteout = 1;
	}

	found = find_filldir_node(buf->rdstate, name, namelen);
	/* If it was found in the table there was a previous whiteout. */
	if (found)
		goto out;

	/*
	 * if it wasn't found and isn't a whiteout, the directory isn't
	 * empty.
	 */
	err = -ENOTEMPTY;
	if ((buf->mode == RD_CHECK_EMPTY) && !whiteout)
		goto out;

	err = add_filldir_node(buf->rdstate, name, namelen,
			       buf->rdstate->bindex, whiteout);

out:
	buf->err = err;
	return err;
}

/* Is a directory logically empty? */
int check_empty(struct dentry *dentry, struct unionfs_dir_state **namelist)
{
	int err = 0;
	struct dentry *lower_dentry = NULL;
	struct super_block *sb;
	struct file *lower_file;
	struct unionfs_rdutil_callback *buf = NULL;
	int bindex, bstart, bend, bopaque;

	sb = dentry->d_sb;


	BUG_ON(!S_ISDIR(dentry->d_inode->i_mode));

	if ((err = unionfs_partial_lookup(dentry)))
		goto out;

	bstart = dbstart(dentry);
	bend = dbend(dentry);
	bopaque = dbopaque(dentry);
	if (0 <= bopaque && bopaque < bend)
		bend = bopaque;

	buf = kmalloc(sizeof(struct unionfs_rdutil_callback), GFP_KERNEL);
	if (!buf) {
		err = -ENOMEM;
		goto out;
	}
	buf->err = 0;
	buf->mode = RD_CHECK_EMPTY;
	buf->rdstate = alloc_rdstate(dentry->d_inode, bstart);
	if (!buf->rdstate) {
		err = -ENOMEM;
		goto out;
	}

	/* Process the lower directories with rdutil_callback as a filldir. */
	for (bindex = bstart; bindex <= bend; bindex++) {
		lower_dentry = unionfs_lower_dentry_idx(dentry, bindex);
		if (!lower_dentry)
			continue;
		if (!lower_dentry->d_inode)
			continue;
		if (!S_ISDIR(lower_dentry->d_inode->i_mode))
			continue;

		dget(lower_dentry);
		unionfs_mntget(dentry, bindex);
		branchget(sb, bindex);
		lower_file =
			dentry_open(lower_dentry,
				    unionfs_lower_mnt_idx(dentry, bindex),
				    O_RDONLY);
		if (IS_ERR(lower_file)) {
			err = PTR_ERR(lower_file);
			dput(lower_dentry);
			branchput(sb, bindex);
			goto out;
		}

		do {
			buf->filldir_called = 0;
			buf->rdstate->bindex = bindex;
			err = vfs_readdir(lower_file,
					  readdir_util_callback, buf);
			if (buf->err)
				err = buf->err;
		} while ((err >= 0) && buf->filldir_called);

		/* fput calls dput for lower_dentry */
		fput(lower_file);
		branchput(sb, bindex);

		if (err < 0)
			goto out;
	}

out:
	if (buf) {
		if (namelist && !err)
			*namelist = buf->rdstate;
		else if (buf->rdstate)
			free_rdstate(buf->rdstate);
		kfree(buf);
	}


	return err;
}
