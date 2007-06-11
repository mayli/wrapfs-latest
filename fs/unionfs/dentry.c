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
 * Revalidate a single dentry.
 * Assume that dentry's info node is locked.
 * Assume that parent(s) are all valid already, but
 * the child may not yet be valid.
 * Returns 1 if valid, 0 otherwise.
 */
static int __unionfs_d_revalidate_one(struct dentry *dentry,
				      struct nameidata *nd)
{
	int valid = 1;		/* default is valid (1); invalid is 0. */
	struct dentry *lower_dentry;
	int bindex, bstart, bend;
	int sbgen, dgen;
	int positive = 0;
	int locked = 0;
	int interpose_flag;
	struct nameidata lowernd; /* TODO: be gentler to the stack */

	if (nd)
		memcpy(&lowernd, nd, sizeof(struct nameidata));
	else
		memset(&lowernd, 0, sizeof(struct nameidata));

	verify_locked(dentry);

	/* if the dentry is unhashed, do NOT revalidate */
	if (d_deleted(dentry)) {
		printk(KERN_DEBUG "unionfs: unhashed dentry being "
		       "revalidated: %*s\n",
		       dentry->d_name.len, dentry->d_name.name);
		goto out;
	}

	BUG_ON(dbstart(dentry) == -1);
	if (dentry->d_inode)
		positive = 1;
	dgen = atomic_read(&UNIONFS_D(dentry)->generation);
	sbgen = atomic_read(&UNIONFS_SB(dentry->d_sb)->generation);
	/*
	 * If we are working on an unconnected dentry, then there is no
	 * revalidation to be done, because this file does not exist within
	 * the namespace, and Unionfs operates on the namespace, not data.
	 */
	if (sbgen != dgen) {
		struct dentry *result;
		int pdgen;

		/* The root entry should always be valid */
		BUG_ON(IS_ROOT(dentry));

		/* We can't work correctly if our parent isn't valid. */
		pdgen = atomic_read(&UNIONFS_D(dentry->d_parent)->generation);
		BUG_ON(pdgen != sbgen);	/* should never happen here */

		/* Free the pointers for our inodes and this dentry. */
		bstart = dbstart(dentry);
		bend = dbend(dentry);
		if (bstart >= 0) {
			struct dentry *lower_dentry;
			for (bindex = bstart; bindex <= bend; bindex++) {
				lower_dentry =
					unionfs_lower_dentry_idx(dentry,
								 bindex);
				dput(lower_dentry);
			}
		}
		set_dbstart(dentry, -1);
		set_dbend(dentry, -1);

		interpose_flag = INTERPOSE_REVAL_NEG;
		if (positive) {
			interpose_flag = INTERPOSE_REVAL;
			/*
			 * During BRM, the VFS could already hold a lock on
			 * a file being read, so don't lock it again
			 * (deadlock), but if you lock it in this function,
			 * then release it here too.
			 */
			if (!mutex_is_locked(&dentry->d_inode->i_mutex)) {
				mutex_lock(&dentry->d_inode->i_mutex);
				locked = 1;
			}

			bstart = ibstart(dentry->d_inode);
			bend = ibend(dentry->d_inode);
			if (bstart >= 0) {
				struct inode *lower_inode;
				for (bindex = bstart; bindex <= bend;
				     bindex++) {
					lower_inode =
						unionfs_lower_inode_idx(
							dentry->d_inode,
							bindex);
					iput(lower_inode);
				}
			}
			kfree(UNIONFS_I(dentry->d_inode)->lower_inodes);
			UNIONFS_I(dentry->d_inode)->lower_inodes = NULL;
			ibstart(dentry->d_inode) = -1;
			ibend(dentry->d_inode) = -1;
			if (locked)
				mutex_unlock(&dentry->d_inode->i_mutex);
		}

		result = unionfs_lookup_backend(dentry, &lowernd,
						interpose_flag);
		if (result) {
			if (IS_ERR(result)) {
				valid = 0;
				goto out;
			}
			/*
			 * current unionfs_lookup_backend() doesn't return
			 * a valid dentry
			 */
			dput(dentry);
			dentry = result;
		}

		if (positive && UNIONFS_I(dentry->d_inode)->stale) {
			make_bad_inode(dentry->d_inode);
			d_drop(dentry);
			valid = 0;
			goto out;
		}
		goto out;
	}

	/* The revalidation must occur across all branches */
	bstart = dbstart(dentry);
	bend = dbend(dentry);
	BUG_ON(bstart == -1);
	for (bindex = bstart; bindex <= bend; bindex++) {
		lower_dentry = unionfs_lower_dentry_idx(dentry, bindex);
		if (!lower_dentry || !lower_dentry->d_op
		    || !lower_dentry->d_op->d_revalidate)
			continue;
		if (!lower_dentry->d_op->d_revalidate(lower_dentry,
						      &lowernd))
			valid = 0;
	}

	if (!dentry->d_inode)
		valid = 0;

	if (valid) {
		/*
		 * If we get here, and we copy the meta-data from the lower
		 * inode to our inode, then it is vital that we have already
		 * purged all unionfs-level file data.  We do that in the
		 * caller (__unionfs_d_revalidate_chain) by calling
		 * purge_inode_data.
		 */
		fsstack_copy_attr_all(dentry->d_inode,
				      unionfs_lower_inode(dentry->d_inode),
				      unionfs_get_nlinks);
		fsstack_copy_inode_size(dentry->d_inode,
					unionfs_lower_inode(dentry->d_inode));
	}

out:
	return valid;
}

/*
 * Determine if the lower inode objects have changed from below the unionfs
 * inode.  Return 1 if changed, 0 otherwise.
 */
static int is_newer_lower(struct dentry *dentry)
{
	int bindex;
	struct inode *inode = dentry->d_inode;
	struct inode *lower_inode;

	if (IS_ROOT(dentry))	/* XXX: root dentry can never be invalid?! */
		return 0;

	if (!inode)
		return 0;

	for (bindex = ibstart(inode); bindex <= ibend(inode); bindex++) {
		lower_inode = unionfs_lower_inode_idx(inode, bindex);
		if (!lower_inode)
			continue;
		/*
		 * We may want to apply other tests to determine if the
		 * lower inode's data has changed, but checking for changed
		 * ctime and mtime on the lower inode should be enough.
		 */
		if (timespec_compare(&inode->i_mtime,
				     &lower_inode->i_mtime) < 0) {
			printk("unionfs: resyncing with lower inode "
			       "(new mtime, name=%s)\n",
			       dentry->d_name.name);
			return 1; /* mtime changed! */
		}
		if (timespec_compare(&inode->i_ctime,
				     &lower_inode->i_ctime) < 0) {
			printk("unionfs: resyncing with lower inode "
			       "(new ctime, name=%s)\n",
			       dentry->d_name.name);
			return 1; /* ctime changed! */
		}
	}
	return 0;		/* default: lower is not newer */
}

/*
 * Purge/remove/unmap all date pages of a unionfs inode.  This is called
 * when the lower inode has changed, and we have to force processes to get
 * the new data.
 *
 * XXX: this function "works" in that as long as a user process will have
 * caused unionfs to be called, directly or indirectly, even to just do
 * ->d_revalidate, then we will have purged the current unionfs data and the
 * process will see the new data.  For example, a process that continually
 * re-reads the same file's data will see the NEW data as soon as the lower
 * file had changed, upon the next read(2) syscall.  However, this doesn't
 * work when the process re-reads the file's data via mmap: once we respond
 * to ->readpage(s), then the kernel maps the page into the process's
 * address space and there doesn't appear to be a way to force the kernel to
 * invalidate those pages/mappings, and force the process to re-issue
 * ->readpage.  If there's a way to invalidate active mappings and force a
 * ->readpage, let us know please (invalidate_inode_pages2 doesn't do the
 * trick).
 */
static inline void purge_inode_data(struct dentry *dentry)
{
	/* reset generation number to zero, guaranteed to be "old" */
	atomic_set(&UNIONFS_D(dentry)->generation, 0);

	/* remove all non-private mappings */
	unmap_mapping_range(dentry->d_inode->i_mapping, 0, 0, 0);

	if (dentry->d_inode->i_data.nrpages)
		truncate_inode_pages(&dentry->d_inode->i_data, 0);
}

/*
 * Revalidate a parent chain of dentries, then the actual node.
 * Assumes that dentry is locked, but will lock all parents if/when needed.
 */
int __unionfs_d_revalidate_chain(struct dentry *dentry, struct nameidata *nd)
{
	int valid = 0;		/* default is invalid (0); valid is 1. */
	struct dentry **chain = NULL; /* chain of dentries to reval */
	int chain_len = 0;
	struct dentry *dtmp;
	int sbgen, dgen, i;
	int saved_bstart, saved_bend, bindex;

	/* find length of chain needed to revalidate */
	/* XXX: should I grab some global (dcache?) lock? */
	chain_len = 0;
	sbgen = atomic_read(&UNIONFS_SB(dentry->d_sb)->generation);
	dtmp = dentry->d_parent;
	if (dtmp->d_inode && is_newer_lower(dtmp)) {
		dgen = 0;
		purge_inode_data(dtmp);
	} else
		dgen = atomic_read(&UNIONFS_D(dtmp)->generation);
	while (sbgen != dgen) {
		/* The root entry should always be valid */
		BUG_ON(IS_ROOT(dtmp));
		chain_len++;
		dtmp = dtmp->d_parent;
		dgen = atomic_read(&UNIONFS_D(dtmp)->generation);
	}
	if (chain_len == 0)
		goto out_this;	/* shortcut if parents are OK */

	/*
	 * Allocate array of dentries to reval.  We could use linked lists,
	 * but the number of entries we need to alloc here is often small,
	 * and short lived, so locality will be better.
	 */
	chain = kzalloc(chain_len * sizeof(struct dentry *), GFP_KERNEL);
	if (!chain) {
		printk("unionfs: no more memory in %s\n", __FUNCTION__);
		goto out;
	}

	/*
	 * lock all dentries in chain, in child to parent order.
	 * if failed, then sleep for a little, then retry.
	 */
	dtmp = dentry->d_parent;
	for (i=chain_len-1; i>=0; i--) {
		chain[i] = dget(dtmp);
		dtmp = dtmp->d_parent;
	}

	/*
	 * call __unionfs_d_revalidate() on each dentry, but in parent to
	 * child order.
	 */
	for (i=0; i<chain_len; i++) {
		unionfs_lock_dentry(chain[i]);
		saved_bstart = dbstart(chain[i]);
		saved_bend = dbend(chain[i]);
		sbgen = atomic_read(&UNIONFS_SB(dentry->d_sb)->generation);
		dgen = atomic_read(&UNIONFS_D(chain[i])->generation);

		valid = __unionfs_d_revalidate_one(chain[i], nd);
		/* XXX: is this the correct mntput condition?! */
		if (valid && chain_len > 0 &&
		    sbgen != dgen && chain[i]->d_inode &&
		    S_ISDIR(chain[i]->d_inode->i_mode)) {
			for (bindex = saved_bstart; bindex <= saved_bend;
			     bindex++)
				unionfs_mntput(chain[i], bindex);
		}
		unionfs_unlock_dentry(chain[i]);

		if (!valid)
			goto out_free;
	}


out_this:
	/* finally, lock this dentry and revalidate it */
	verify_locked(dentry);
	if (dentry->d_inode && is_newer_lower(dentry)) {
		dgen = 0;
		purge_inode_data(dentry);
	} else
		dgen = atomic_read(&UNIONFS_D(dentry)->generation);
	valid = __unionfs_d_revalidate_one(dentry, nd);

	/*
	 * If __unionfs_d_revalidate_one() succeeded above, then it will
	 * have incremented the refcnt of the mnt's, but also the branch
	 * indices of the dentry will have been updated (to take into
	 * account any branch insertions/deletion.  So the current
	 * dbstart/dbend match the current, and new, indices of the mnts
	 * which __unionfs_d_revalidate_one has incremented.  Note: the "if"
	 * test below does not depend on whether chain_len was 0 or greater.
	 */
	if (valid && sbgen != dgen)
		for (bindex = dbstart(dentry);
		     bindex <= dbend(dentry);
		     bindex++)
			unionfs_mntput(dentry, bindex);

out_free:
	/* unlock/dput all dentries in chain and return status */
	if (chain_len > 0) {
		for (i=0; i<chain_len; i++)
			dput(chain[i]);
		kfree(chain);
	}
out:
	return valid;
}

static int unionfs_d_revalidate(struct dentry *dentry, struct nameidata *nd)
{
	int err;

	unionfs_check_dentry(dentry);
	unionfs_lock_dentry(dentry);
	err = __unionfs_d_revalidate_chain(dentry, nd);
	unionfs_unlock_dentry(dentry);
	unionfs_check_dentry(dentry);

	return err;
}

/*
 * At this point no one can reference this dentry, so we don't have to be
 * careful about concurrent access.
 */
static void unionfs_d_release(struct dentry *dentry)
{
	int bindex, bstart, bend;

	unionfs_check_dentry(dentry);
	/* this could be a negative dentry, so check first */
	if (!UNIONFS_D(dentry)) {
		printk(KERN_DEBUG "unionfs: dentry without private data: %.*s",
		       dentry->d_name.len, dentry->d_name.name);
		goto out;
	} else if (dbstart(dentry) < 0) {
		/* this is due to a failed lookup */
		printk(KERN_DEBUG "unionfs: dentry without lower "
		       "dentries: %.*s",
		       dentry->d_name.len, dentry->d_name.name);
		goto out_free;
	}

	/* Release all the lower dentries */
	bstart = dbstart(dentry);
	bend = dbend(dentry);
	for (bindex = bstart; bindex <= bend; bindex++) {
		dput(unionfs_lower_dentry_idx(dentry, bindex));
		unionfs_set_lower_dentry_idx(dentry, bindex, NULL);
		/* NULL lower mnt is ok if this is a negative dentry */
		if (!dentry->d_inode && !unionfs_lower_mnt_idx(dentry,bindex))
			continue;
		unionfs_mntput(dentry, bindex);
		unionfs_set_lower_mnt_idx(dentry, bindex, NULL);
	}
	/* free private data (unionfs_dentry_info) here */
	kfree(UNIONFS_D(dentry)->lower_paths);
	UNIONFS_D(dentry)->lower_paths = NULL;

out_free:
	/* No need to unlock it, because it is disappeared. */
	free_dentry_private_data(UNIONFS_D(dentry));
	dentry->d_fsdata = NULL;	/* just to be safe */

out:
	return;
}

struct dentry_operations unionfs_dops = {
	.d_revalidate	= unionfs_d_revalidate,
	.d_release	= unionfs_d_release,
};
