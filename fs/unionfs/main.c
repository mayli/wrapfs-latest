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
#include <linux/module.h>
#include <linux/moduleparam.h>

/*
 * Connect a unionfs inode dentry/inode with several lower ones.  This is
 * the classic stackable file system "vnode interposition" action.
 *
 * @sb: unionfs's super_block
 */
struct dentry *unionfs_interpose(struct dentry *dentry, struct super_block *sb,
				 int flag)
{
	struct inode *hidden_inode;
	struct dentry *hidden_dentry;
	int err = 0;
	struct inode *inode;
	int is_negative_dentry = 1;
	int bindex, bstart, bend;
	int skipped = 1;
	struct dentry *spliced = NULL;

	verify_locked(dentry);

	bstart = dbstart(dentry);
	bend = dbend(dentry);

	/* Make sure that we didn't get a negative dentry. */
	for (bindex = bstart; bindex <= bend; bindex++) {
		if (unionfs_lower_dentry_idx(dentry, bindex) &&
		    unionfs_lower_dentry_idx(dentry, bindex)->d_inode) {
			is_negative_dentry = 0;
			break;
		}
	}
	BUG_ON(is_negative_dentry);

	/*
	 * We allocate our new inode below, by calling iget.
	 * iget will call our read_inode which will initialize some
	 * of the new inode's fields
	 */

	/*
	 * On revalidate we've already got our own inode and just need
	 * to fix it up.
	 */
	if (flag == INTERPOSE_REVAL) {
		inode = dentry->d_inode;
		UNIONFS_I(inode)->bstart = -1;
		UNIONFS_I(inode)->bend = -1;
		atomic_set(&UNIONFS_I(inode)->generation,
			   atomic_read(&UNIONFS_SB(sb)->generation));

		UNIONFS_I(inode)->lower_inodes =
			kcalloc(sbmax(sb), sizeof(struct inode *), GFP_KERNEL);
		if (!UNIONFS_I(inode)->lower_inodes) {
			err = -ENOMEM;
			goto out;
		}
	} else {
		/* get unique inode number for unionfs */
		inode = iget(sb, iunique(sb, UNIONFS_ROOT_INO));
		if (!inode) {
			err = -EACCES;
			goto out;
		}
		if (atomic_read(&inode->i_count) > 1)
			goto skip;
	}

fill_i_info:
	skipped = 0;
	for (bindex = bstart; bindex <= bend; bindex++) {
		hidden_dentry = unionfs_lower_dentry_idx(dentry, bindex);
		if (!hidden_dentry) {
			unionfs_set_lower_inode_idx(inode, bindex, NULL);
			continue;
		}

		/* Initialize the hidden inode to the new hidden inode. */
		if (!hidden_dentry->d_inode)
			continue;

		unionfs_set_lower_inode_idx(inode, bindex,
					    igrab(hidden_dentry->d_inode));
	}

	ibstart(inode) = dbstart(dentry);
	ibend(inode) = dbend(dentry);

	/* Use attributes from the first branch. */
	hidden_inode = unionfs_lower_inode(inode);

	/* Use different set of inode ops for symlinks & directories */
	if (S_ISLNK(hidden_inode->i_mode))
		inode->i_op = &unionfs_symlink_iops;
	else if (S_ISDIR(hidden_inode->i_mode))
		inode->i_op = &unionfs_dir_iops;

	/* Use different set of file ops for directories */
	if (S_ISDIR(hidden_inode->i_mode))
		inode->i_fop = &unionfs_dir_fops;

	/* properly initialize special inodes */
	if (S_ISBLK(hidden_inode->i_mode) || S_ISCHR(hidden_inode->i_mode) ||
	    S_ISFIFO(hidden_inode->i_mode) || S_ISSOCK(hidden_inode->i_mode))
		init_special_inode(inode, hidden_inode->i_mode,
				   hidden_inode->i_rdev);

	/* all well, copy inode attributes */
	fsstack_copy_attr_all(inode, hidden_inode, unionfs_get_nlinks);
	fsstack_copy_inode_size(inode, hidden_inode);

	if (spliced)
		goto out_spliced;
skip:
	/* only (our) lookup wants to do a d_add */
	switch (flag) {
	case INTERPOSE_DEFAULT:
	case INTERPOSE_REVAL_NEG:
		d_instantiate(dentry, inode);
		break;
	case INTERPOSE_LOOKUP:
		spliced = d_splice_alias(inode, dentry);
		if (IS_ERR(spliced))
			err = PTR_ERR(spliced);

		/*
		 * d_splice can return a dentry if it was disconnected and
		 * had to be moved.  We must ensure that the private data of
		 * the new dentry is correct and that the inode info was
		 * filled properly.  Finally we must return this new dentry.
		 */
		else if (spliced && spliced != dentry) {
			spliced->d_op = &unionfs_dops;
			spliced->d_fsdata = dentry->d_fsdata;
			dentry->d_fsdata = NULL;
			dentry = spliced;
			if (skipped)
				goto fill_i_info;
			goto out_spliced;
		}
		break;
	case INTERPOSE_REVAL:
		/* Do nothing. */
		break;
	default:
		printk(KERN_ERR "unionfs: invalid interpose flag passed!");
		BUG();
	}
	goto out;

out_spliced:
	if (!err)
		return spliced;
out:
	return ERR_PTR(err);
}

/* like interpose above, but for an already existing dentry */
void unionfs_reinterpose(struct dentry *dentry)
{
	struct dentry *hidden_dentry;
	struct inode *inode;
	int bindex, bstart, bend;

	verify_locked(dentry);

	/* This is pre-allocated inode */
	inode = dentry->d_inode;

	bstart = dbstart(dentry);
	bend = dbend(dentry);
	for (bindex = bstart; bindex <= bend; bindex++) {
		hidden_dentry = unionfs_lower_dentry_idx(dentry, bindex);
		if (!hidden_dentry)
			continue;

		if (!hidden_dentry->d_inode)
			continue;
		if (unionfs_lower_inode_idx(inode, bindex))
			continue;
		unionfs_set_lower_inode_idx(inode, bindex,
					    igrab(hidden_dentry->d_inode));
	}
	ibstart(inode) = dbstart(dentry);
	ibend(inode) = dbend(dentry);
}

/*
 * make sure the branch we just looked up (nd) makes sense:
 *
 * 1) we're not trying to stack unionfs on top of unionfs
 * 2) it exists
 * 3) is a directory
 */
int check_branch(struct nameidata *nd)
{
	if (!strcmp(nd->dentry->d_sb->s_type->name, "unionfs"))
		return -EINVAL;
	if (!nd->dentry->d_inode)
		return -ENOENT;
	if (!S_ISDIR(nd->dentry->d_inode->i_mode))
		return -ENOTDIR;
	return 0;
}

/* checks if two hidden_dentries have overlapping branches */
static int is_branch_overlap(struct dentry *dent1, struct dentry *dent2)
{
	struct dentry *dent = NULL;

	dent = dent1;
	while ((dent != dent2) && (dent->d_parent != dent))
		dent = dent->d_parent;

	if (dent == dent2)
		return 1;

	dent = dent2;
	while ((dent != dent1) && (dent->d_parent != dent))
		dent = dent->d_parent;

	return (dent == dent1);
}

/*
 * Parse branch mode helper function
 */
int __parse_branch_mode(const char *name)
{
	if (!name)
		return 0;
	if (!strcmp(name, "ro"))
		return MAY_READ;
	if (!strcmp(name, "rw"))
		return (MAY_READ | MAY_WRITE);
	return 0;
}

/*
 * Parse "ro" or "rw" options, but default to "rw" of no mode options
 * was specified.
 */
int parse_branch_mode(const char *name)
{
	int perms =  __parse_branch_mode(name);

	if (perms == 0)
		perms = MAY_READ | MAY_WRITE;
	return perms;
}

/* parse the dirs= mount argument */
static int parse_dirs_option(struct super_block *sb, struct unionfs_dentry_info
			     *hidden_root_info, char *options)
{
	struct nameidata nd;
	char *name;
	int err = 0;
	int branches = 1;
	int bindex = 0;
	int i = 0;
	int j = 0;
	struct dentry *dent1;
	struct dentry *dent2;

	if (options[0] == '\0') {
		printk(KERN_WARNING "unionfs: no branches specified\n");
		err = -EINVAL;
		goto out;
	}

	/*
	 * Each colon means we have a separator, this is really just a rough
	 * guess, since strsep will handle empty fields for us.
	 */
	for (i = 0; options[i]; i++)
		if (options[i] == ':')
			branches++;

	/* allocate space for underlying pointers to hidden dentry */
	UNIONFS_SB(sb)->data =
		kcalloc(branches, sizeof(struct unionfs_data), GFP_KERNEL);
	if (!UNIONFS_SB(sb)->data) {
		err = -ENOMEM;
		goto out;
	}

	hidden_root_info->lower_paths =
		kcalloc(branches, sizeof(struct path), GFP_KERNEL);
	if (!hidden_root_info->lower_paths) {
		err = -ENOMEM;
		goto out;
	}

	/* now parsing a string such as "b1:b2=rw:b3=ro:b4" */
	branches = 0;
	while ((name = strsep(&options, ":")) != NULL) {
		int perms;
		char *mode = strchr(name, '=');

		if (!name || !*name)
			continue;

		branches++;

		/* strip off '=' if any */
		if (mode)
			*mode++ = '\0';

		perms = parse_branch_mode(mode);
		if (!bindex && !(perms & MAY_WRITE)) {
			err = -EINVAL;
			goto out;
		}

		err = path_lookup(name, LOOKUP_FOLLOW, &nd);
		if (err) {
			printk(KERN_WARNING "unionfs: error accessing "
			       "hidden directory '%s' (error %d)\n",
			       name, err);
			goto out;
		}

		if ((err = check_branch(&nd))) {
			printk(KERN_WARNING "unionfs: hidden directory "
			       "'%s' is not a valid branch\n", name);
			path_release(&nd);
			goto out;
		}

		hidden_root_info->lower_paths[bindex].dentry = nd.dentry;
		hidden_root_info->lower_paths[bindex].mnt = nd.mnt;

		unionfs_write_lock(sb);
		set_branchperms(sb, bindex, perms);
		set_branch_count(sb, bindex, 0);
		new_branch_id(sb, bindex);
		unionfs_write_unlock(sb);

		if (hidden_root_info->bstart < 0)
			hidden_root_info->bstart = bindex;
		hidden_root_info->bend = bindex;
		bindex++;
	}

	if (branches == 0) {
		printk(KERN_WARNING "unionfs: no branches specified\n");
		err = -EINVAL;
		goto out;
	}

	BUG_ON(branches != (hidden_root_info->bend + 1));

	/*
	 * Ensure that no overlaps exist in the branches.
	 *
	 * This test is required because the Linux kernel has no support
	 * currently for ensuring coherency between stackable layers and
	 * branches.  If we were to allow overlapping branches, it would be
	 * possible, for example, to delete a file via one branch, which
	 * would not be reflected in another branch.  Such incoherency could
	 * lead to inconsistencies and even kernel oopses.  Rather than
	 * implement hacks to work around some of these cache-coherency
	 * problems, we prevent branch overlapping, for now.  A complete
	 * solution will involve proper kernel/VFS support for cache
	 * coherency, at which time we could safely remove this
	 * branch-overlapping test.
	 */
	for (i = 0; i < branches; i++) {
		dent1 = hidden_root_info->lower_paths[i].dentry;
		for (j = i + 1; j < branches; j++) {
			dent2 = hidden_root_info->lower_paths[j].dentry;
			if (is_branch_overlap(dent1, dent2)) {
				printk(KERN_WARNING "unionfs: branches %d and "
				       "%d overlap\n", i, j);
				err = -EINVAL;
				goto out;
			}
		}
	}

out:
	if (err) {
		for (i = 0; i < branches; i++)
			if (hidden_root_info->lower_paths[i].dentry) {
				dput(hidden_root_info->lower_paths[i].dentry);
				/* initialize: can't use unionfs_mntput here */
				mntput(hidden_root_info->lower_paths[i].mnt);
			}

		kfree(hidden_root_info->lower_paths);
		kfree(UNIONFS_SB(sb)->data);

		/*
		 * MUST clear the pointers to prevent potential double free if
		 * the caller dies later on
		 */
		hidden_root_info->lower_paths = NULL;
		UNIONFS_SB(sb)->data = NULL;
	}
	return err;
}

/*
 * Parse mount options.  See the manual page for usage instructions.
 *
 * Returns the dentry object of the lower-level (hidden) directory;
 * We want to mount our stackable file system on top of that hidden directory.
 */
static struct unionfs_dentry_info *unionfs_parse_options(
					struct super_block *sb,
					char *options)
{
	struct unionfs_dentry_info *hidden_root_info;
	char *optname;
	int err = 0;
	int bindex;
	int dirsfound = 0;

	/* allocate private data area */
	err = -ENOMEM;
	hidden_root_info =
		kzalloc(sizeof(struct unionfs_dentry_info), GFP_KERNEL);
	if (!hidden_root_info)
		goto out_error;
	hidden_root_info->bstart = -1;
	hidden_root_info->bend = -1;
	hidden_root_info->bopaque = -1;

	while ((optname = strsep(&options, ",")) != NULL) {
		char *optarg;
		char *endptr;
		int intval;

		if (!optname || !*optname)
			continue;

		optarg = strchr(optname, '=');
		if (optarg)
			*optarg++ = '\0';

		/*
		 * All of our options take an argument now. Insert ones that
		 * don't, above this check.
		 */
		if (!optarg) {
			printk("unionfs: %s requires an argument.\n", optname);
			err = -EINVAL;
			goto out_error;
		}

		if (!strcmp("dirs", optname)) {
			if (++dirsfound > 1) {
				printk(KERN_WARNING
				       "unionfs: multiple dirs specified\n");
				err = -EINVAL;
				goto out_error;
			}
			err = parse_dirs_option(sb, hidden_root_info, optarg);
			if (err)
				goto out_error;
			continue;
		}

		/* All of these options require an integer argument. */
		intval = simple_strtoul(optarg, &endptr, 0);
		if (*endptr) {
			printk(KERN_WARNING
			       "unionfs: invalid %s option '%s'\n",
			       optname, optarg);
			err = -EINVAL;
			goto out_error;
		}

		err = -EINVAL;
		printk(KERN_WARNING
		       "unionfs: unrecognized option '%s'\n", optname);
		goto out_error;
	}
	if (dirsfound != 1) {
		printk(KERN_WARNING "unionfs: dirs option required\n");
		err = -EINVAL;
		goto out_error;
	}
	goto out;

out_error:
	if (hidden_root_info && hidden_root_info->lower_paths) {
		for (bindex = hidden_root_info->bstart;
		     bindex >= 0 && bindex <= hidden_root_info->bend;
		     bindex++) {
			struct dentry *d;
			struct vfsmount *m;

			d = hidden_root_info->lower_paths[bindex].dentry;
			m = hidden_root_info->lower_paths[bindex].mnt;

			dput(d);
			/* initializing: can't use unionfs_mntput here */
			mntput(m);
		}
	}

	kfree(hidden_root_info->lower_paths);
	kfree(hidden_root_info);

	kfree(UNIONFS_SB(sb)->data);
	UNIONFS_SB(sb)->data = NULL;

	hidden_root_info = ERR_PTR(err);
out:
	return hidden_root_info;
}

/*
 * our custom d_alloc_root work-alike
 *
 * we can't use d_alloc_root if we want to use our own interpose function
 * unchanged, so we simply call our own "fake" d_alloc_root
 */
static struct dentry *unionfs_d_alloc_root(struct super_block *sb)
{
	struct dentry *ret = NULL;

	if (sb) {
		static const struct qstr name = {.name = "/",.len = 1 };

		ret = d_alloc(NULL, &name);
		if (ret) {
			ret->d_op = &unionfs_dops;
			ret->d_sb = sb;
			ret->d_parent = ret;
		}
	}
	return ret;
}

static int unionfs_read_super(struct super_block *sb, void *raw_data,
			      int silent)
{
	int err = 0;
	struct unionfs_dentry_info *hidden_root_info = NULL;
	int bindex, bstart, bend;

	if (!raw_data) {
		printk(KERN_WARNING
		       "unionfs: read_super: missing data argument\n");
		err = -EINVAL;
		goto out;
	}

	/* Allocate superblock private data */
	sb->s_fs_info = kzalloc(sizeof(struct unionfs_sb_info), GFP_KERNEL);
	if (!UNIONFS_SB(sb)) {
		printk(KERN_WARNING "unionfs: read_super: out of memory\n");
		err = -ENOMEM;
		goto out;
	}

	UNIONFS_SB(sb)->bend = -1;
	atomic_set(&UNIONFS_SB(sb)->generation, 1);
	init_rwsem(&UNIONFS_SB(sb)->rwsem);
	UNIONFS_SB(sb)->high_branch_id = -1; /* -1 == invalid branch ID */

	hidden_root_info = unionfs_parse_options(sb, raw_data);
	if (IS_ERR(hidden_root_info)) {
		printk(KERN_WARNING
		       "unionfs: read_super: error while parsing options "
		       "(err = %ld)\n", PTR_ERR(hidden_root_info));
		err = PTR_ERR(hidden_root_info);
		hidden_root_info = NULL;
		goto out_free;
	}
	if (hidden_root_info->bstart == -1) {
		err = -ENOENT;
		goto out_free;
	}

	/* set the hidden superblock field of upper superblock */
	bstart = hidden_root_info->bstart;
	BUG_ON(bstart != 0);
	sbend(sb) = bend = hidden_root_info->bend;
	for (bindex = bstart; bindex <= bend; bindex++) {
		struct dentry *d;

		d = hidden_root_info->lower_paths[bindex].dentry;

		unionfs_write_lock(sb);
		unionfs_set_lower_super_idx(sb, bindex, d->d_sb);
		unionfs_write_unlock(sb);
	}

	/* max Bytes is the maximum bytes from highest priority branch */
	unionfs_read_lock(sb);
	sb->s_maxbytes = unionfs_lower_super_idx(sb, 0)->s_maxbytes;
	unionfs_read_unlock(sb);

	sb->s_op = &unionfs_sops;

	/* See comment next to the definition of unionfs_d_alloc_root */
	sb->s_root = unionfs_d_alloc_root(sb);
	if (!sb->s_root) {
		err = -ENOMEM;
		goto out_dput;
	}

	/* link the upper and lower dentries */
	sb->s_root->d_fsdata = NULL;
	if ((err = new_dentry_private_data(sb->s_root)))
		goto out_freedpd;

	/* Set the hidden dentries for s_root */
	for (bindex = bstart; bindex <= bend; bindex++) {
		struct dentry *d;
		struct vfsmount *m;

		d = hidden_root_info->lower_paths[bindex].dentry;
		m = hidden_root_info->lower_paths[bindex].mnt;

		unionfs_set_lower_dentry_idx(sb->s_root, bindex, d);
		unionfs_set_lower_mnt_idx(sb->s_root, bindex, m);
	}
	set_dbstart(sb->s_root, bstart);
	set_dbend(sb->s_root, bend);

	/* Set the generation number to one, since this is for the mount. */
	atomic_set(&UNIONFS_D(sb->s_root)->generation, 1);

	/*
	 * Call interpose to create the upper level inode.  Only
	 * INTERPOSE_LOOKUP can return a value other than 0 on err.
	 */
	err = PTR_ERR(unionfs_interpose(sb->s_root, sb, 0));
	unionfs_unlock_dentry(sb->s_root);
	if (!err)
		goto out;
	/* else fall through */

out_freedpd:
	if (UNIONFS_D(sb->s_root)) {
		kfree(UNIONFS_D(sb->s_root)->lower_paths);
		free_dentry_private_data(UNIONFS_D(sb->s_root));
	}
	dput(sb->s_root);

out_dput:
	if (hidden_root_info && !IS_ERR(hidden_root_info)) {
		for (bindex = hidden_root_info->bstart;
		     bindex <= hidden_root_info->bend; bindex++) {
			struct dentry *d;
			struct vfsmount *m;

			d = hidden_root_info->lower_paths[bindex].dentry;
			m = hidden_root_info->lower_paths[bindex].mnt;

			dput(d);
			/* initializing: can't use unionfs_mntput here */
			mntput(m);
		}
		kfree(hidden_root_info->lower_paths);
		kfree(hidden_root_info);
		hidden_root_info = NULL;
	}

out_free:
	kfree(UNIONFS_SB(sb)->data);
	kfree(UNIONFS_SB(sb));
	sb->s_fs_info = NULL;

out:
	if (hidden_root_info && !IS_ERR(hidden_root_info)) {
		kfree(hidden_root_info->lower_paths);
		kfree(hidden_root_info);
	}
	return err;
}

static int unionfs_get_sb(struct file_system_type *fs_type,
			  int flags, const char *dev_name,
			  void *raw_data, struct vfsmount *mnt)
{
	return get_sb_nodev(fs_type, flags, raw_data, unionfs_read_super, mnt);
}

static struct file_system_type unionfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "unionfs",
	.get_sb		= unionfs_get_sb,
	.kill_sb	= generic_shutdown_super,
	.fs_flags	= FS_REVAL_DOT,
};

static int __init init_unionfs_fs(void)
{
	int err;

	printk("Registering unionfs " UNIONFS_VERSION "\n");

	if ((err = unionfs_init_filldir_cache()))
		goto out;
	if ((err = unionfs_init_inode_cache()))
		goto out;
	if ((err = unionfs_init_dentry_cache()))
		goto out;
	if ((err = init_sioq()))
		goto out;
	err = register_filesystem(&unionfs_fs_type);
out:
	if (err) {
		stop_sioq();
		unionfs_destroy_filldir_cache();
		unionfs_destroy_inode_cache();
		unionfs_destroy_dentry_cache();
	}
	return err;
}

static void __exit exit_unionfs_fs(void)
{
	stop_sioq();
	unionfs_destroy_filldir_cache();
	unionfs_destroy_inode_cache();
	unionfs_destroy_dentry_cache();
	unregister_filesystem(&unionfs_fs_type);
	printk("Completed unionfs module unload.\n");
}

MODULE_AUTHOR("Erez Zadok, Filesystems and Storage Lab, Stony Brook University"
	      " (http://www.fsl.cs.sunysb.edu)");
MODULE_DESCRIPTION("Unionfs " UNIONFS_VERSION
		   " (http://unionfs.filesystems.org)");
MODULE_LICENSE("GPL");

module_init(init_unionfs_fs);
module_exit(exit_unionfs_fs);
