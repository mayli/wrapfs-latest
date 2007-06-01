/*
 * Copyright (c) 2003-2007 Erez Zadok
 * Copyright (c) 2005-2007 Josef 'Jeff' Sipek
 * Copyright (c) 2003-2007 Stony Brook University
 * Copyright (c) 2003-2007 The Research Foundation of SUNY
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "union.h"

/*
 * Helper debugging functions for maintainers (and for users to report back
 * useful information back to maintainers)
 */

/* it's always useful to know what part of the code called us */
#define PRINT_CALLER()				\
do {						\
  if (!printed_caller) {			\
    printk("PC:%s:%s:%d\n",fname,fxn,line);	\
    printed_caller = 1;				\
  }						\
 } while (0)

/*
 * __unionfs_check_{inode,dentry,file} perform exhaustive sanity checking on
 * the fan-out of various Unionfs objects.  We check that no lower objects
 * exist  outside the start/end branch range; that all objects within are
 * non-NULL (with some allowed exceptions); that for every lower file
 * there's a lower dentry+inode; that the start/end ranges match for all
 * corresponding lower objects; that open files/symlinks have only one lower
 * objects, but directories can have several; and more.
 */
void __unionfs_check_inode(const struct inode *inode,
			   const char *fname, const char *fxn, int line)
{
	int bindex;
	int istart, iend;
	struct inode *lower_inode;
	struct super_block *sb;
	int printed_caller = 0;

	/* for inodes now */
	BUG_ON(!inode);
	sb = inode->i_sb;
	istart = ibstart(inode);
	iend = ibend(inode);
	if (istart > iend) {
		PRINT_CALLER();
		printk(" Ci0: inode=%p istart/end=%d:%d\n",
		       inode, istart, iend);
	}
	if ((istart == -1 && iend != -1) ||
	    (istart != -1 && iend == -1)) {
		PRINT_CALLER();
		printk(" Ci1: inode=%p istart/end=%d:%d\n",
		       inode, istart, iend);
	}
	if (!S_ISDIR(inode->i_mode)) {
		if (iend != istart) {
			PRINT_CALLER();
			printk(" Ci2: inode=%p istart=%d iend=%d\n",
			       inode, istart, iend);
		}
	}

	for (bindex = sbstart(sb); bindex < sbmax(sb); bindex++) {
		if (!UNIONFS_I(inode)) {
			PRINT_CALLER();
			printk(" Ci3: no inode_info %p\n", inode);
			return;
		}
		if (!UNIONFS_I(inode)->lower_inodes) {
			PRINT_CALLER();
			printk(" Ci4: no lower_inodes %p\n", inode);
			return;
		}
		lower_inode = unionfs_lower_inode_idx(inode, bindex);
		if (lower_inode) {
			if (bindex < istart || bindex > iend) {
				PRINT_CALLER();
				printk(" Ci5: inode/linode=%p:%p bindex=%d "
				       "istart/end=%d:%d\n", inode,
				       lower_inode, bindex, istart, iend);
			} else if ((int)lower_inode == 0x5a5a5a5a) {
				/* freed inode! */
				PRINT_CALLER();
				printk(" Ci6: inode/linode=%p:%p bindex=%d "
				       "istart/end=%d:%d\n", inode,
				       lower_inode, bindex, istart, iend);
			}
		} else {	/* lower_inode == NULL */
			if (bindex >= istart && bindex <= iend) {
				/*
				 * directories can have NULL lower inodes in
				 * b/t start/end, but NOT if at the
				 * start/end range.
				 */
				if (!(S_ISDIR(inode->i_mode) &&
				      bindex > istart && bindex < iend)) {
					PRINT_CALLER();
					printk(" Ci7: inode/linode=%p:%p "
					       "bindex=%d istart/end=%d:%d\n",
					       inode, lower_inode, bindex,
					       istart, iend);
				}
			}
		}
	}
}

void __unionfs_check_dentry(const struct dentry *dentry,
			    const char *fname, const char *fxn, int line)
{
	int bindex;
	int dstart, dend, istart, iend;
	struct dentry *lower_dentry;
	struct inode *inode, *lower_inode;
	struct super_block *sb;
	struct vfsmount *lower_mnt;
	int printed_caller = 0;

	BUG_ON(!dentry);
	sb = dentry->d_sb;
	inode = dentry->d_inode;
	dstart = dbstart(dentry);
	dend = dbend(dentry);
	BUG_ON(dstart > dend);

	if ((dstart == -1 && dend != -1) ||
	    (dstart != -1 && dend == -1)) {
		PRINT_CALLER();
		printk(" CD0: dentry=%p dstart/end=%d:%d\n",
		       dentry, dstart, dend);
	}
	/*
	 * check for NULL dentries inside the start/end range, or
	 * non-NULL dentries outside the start/end range.
	 */
	for (bindex = sbstart(sb); bindex < sbmax(sb); bindex++) {
		lower_dentry = unionfs_lower_dentry_idx(dentry, bindex);
		if (lower_dentry) {
			if (bindex < dstart || bindex > dend) {
				PRINT_CALLER();
				printk(" CD1: dentry/lower=%p:%p(%p) "
				       "bindex=%d dstart/end=%d:%d\n",
				       dentry, lower_dentry,
				       (lower_dentry ? lower_dentry->d_inode :
					(void *) 0xffffffff),
				       bindex, dstart, dend);
			}
		} else {	/* lower_dentry == NULL */
			if (bindex >= dstart && bindex <= dend) {
				/*
				 * Directories can have NULL lower inodes in
				 * b/t start/end, but NOT if at the
				 * start/end range.  Ignore this rule,
				 * however, if this is a NULL dentry or a
				 * deleted dentry.
				 */
				if (!d_deleted((struct dentry *) dentry) &&
				    inode &&
				    !(inode && S_ISDIR(inode->i_mode) &&
				      bindex > dstart && bindex < dend)) {
					PRINT_CALLER();
					printk(" CD2: dentry/lower=%p:%p(%p) "
					       "bindex=%d dstart/end=%d:%d\n",
					       dentry, lower_dentry,
					       (lower_dentry ?
						lower_dentry->d_inode :
						(void *) 0xffffffff),
					       bindex, dstart, dend);
				}
			}
		}
	}

	/* check for vfsmounts same as for dentries */
	for (bindex = sbstart(sb); bindex < sbmax(sb); bindex++) {
		lower_mnt = unionfs_lower_mnt_idx(dentry, bindex);
		if (lower_mnt) {
			if (bindex < dstart || bindex > dend) {
				PRINT_CALLER();
				printk(" CM0: dentry/lmnt=%p:%p bindex=%d "
				       "dstart/end=%d:%d\n", dentry,
				       lower_mnt, bindex, dstart, dend);
			}
		} else {	/* lower_mnt == NULL */
			if (bindex >= dstart && bindex <= dend) {
				/*
				 * Directories can have NULL lower inodes in
				 * b/t start/end, but NOT if at the
				 * start/end range.  Ignore this rule,
				 * however, if this is a NULL dentry.
				 */
				if (inode &&
				    !(inode && S_ISDIR(inode->i_mode) &&
				      bindex > dstart && bindex < dend)) {
					PRINT_CALLER();
					printk(" CM1: dentry/lmnt=%p:%p "
					       "bindex=%d dstart/end=%d:%d\n",
					       dentry, lower_mnt, bindex,
					       dstart, dend);
				}
			}
		}
	}

	/* for inodes now */
	if (!inode)
		return;
	istart = ibstart(inode);
	iend = ibend(inode);
	BUG_ON(istart > iend);
	if ((istart == -1 && iend != -1) ||
	    (istart != -1 && iend == -1)) {
		PRINT_CALLER();
		printk(" CI0: dentry/inode=%p:%p istart/end=%d:%d\n",
		       dentry, inode, istart, iend);
	}
	if (istart != dstart) {
		PRINT_CALLER();
		printk(" CI1: dentry/inode=%p:%p istart=%d dstart=%d\n",
		       dentry, inode, istart, dstart);
	}
	if (iend != dend) {
		PRINT_CALLER();
		printk(" CI2: dentry/inode=%p:%p iend=%d dend=%d\n",
		       dentry, inode, iend, dend);
	}

	if (!S_ISDIR(inode->i_mode)) {
		if (dend != dstart) {
			PRINT_CALLER();
			printk(" CI3: dentry/inode=%p:%p dstart=%d dend=%d\n",
			       dentry, inode, dstart, dend);
		}
		if (iend != istart) {
			PRINT_CALLER();
			printk(" CI4: dentry/inode=%p:%p istart=%d iend=%d\n",
			       dentry, inode, istart, iend);
		}
	}

	for (bindex = sbstart(sb); bindex < sbmax(sb); bindex++) {
		lower_inode = unionfs_lower_inode_idx(inode, bindex);
		if (lower_inode) {
			if (bindex < istart || bindex > iend) {
				PRINT_CALLER();
				printk(" CI5: dentry/linode=%p:%p bindex=%d "
				       "istart/end=%d:%d\n", dentry,
				       lower_inode, bindex, istart, iend);
			} else if ((int)lower_inode == 0x5a5a5a5a) {
				/* freed inode! */
				PRINT_CALLER();
				printk(" CI6: dentry/linode=%p:%p bindex=%d "
				       "istart/end=%d:%d\n", dentry,
				       lower_inode, bindex, istart, iend);
			}
		} else {	/* lower_inode == NULL */
			if (bindex >= istart && bindex <= iend) {
				/*
				 * directories can have NULL lower inodes in
				 * b/t start/end, but NOT if at the
				 * start/end range.
				 */
				if (!(S_ISDIR(inode->i_mode) &&
				      bindex > istart && bindex < iend)) {
					PRINT_CALLER();
					printk(" CI7: dentry/linode=%p:%p "
					       "bindex=%d istart/end=%d:%d\n",
					       dentry, lower_inode, bindex,
					       istart, iend);
				}
			}
		}
	}

	/*
	 * If it's a directory, then intermediate objects b/t start/end can
	 * be NULL.  But, check that all three are NULL: lower dentry, mnt,
	 * and inode.
	 */
	if (S_ISDIR(inode->i_mode))
		for (bindex = dstart+1; bindex < dend-1; bindex++) {
			lower_inode = unionfs_lower_inode_idx(inode, bindex);
			lower_dentry = unionfs_lower_dentry_idx(dentry,
								bindex);
			lower_mnt = unionfs_lower_mnt_idx(dentry, bindex);
			if (!((lower_inode && lower_dentry && lower_mnt) ||
			      (!lower_inode && !lower_dentry && !lower_mnt)))
				printk(" Cx: lmnt/ldentry/linode=%p:%p:%p "
				       "bindex=%d dstart/end=%d:%d\n",
				       lower_mnt, lower_dentry, lower_inode,
				       bindex, dstart, dend);
		}
}

void __unionfs_check_file(const struct file *file,
			  const char *fname, const char *fxn, int line)
{
	int bindex;
	int dstart, dend, fstart, fend;
	struct dentry *dentry;
	struct file *lower_file;
	struct inode *inode;
	struct super_block *sb;
	int printed_caller = 0;

	BUG_ON(!file);
	dentry = file->f_dentry;
	sb = dentry->d_sb;
	dstart = dbstart(dentry);
	dend = dbend(dentry);
	BUG_ON(dstart > dend);
	fstart = fbstart(file);
	fend = fbend(file);
	BUG_ON(fstart > fend);

	if ((fstart == -1 && fend != -1) ||
	    (fstart != -1 && fend == -1)) {
		PRINT_CALLER();
		printk(" CF0: file/dentry=%p:%p fstart/end=%d:%d\n",
		       file, dentry, fstart, fend);
	}
	if (fstart != dstart) {
		PRINT_CALLER();
		printk(" CF1: file/dentry=%p:%p fstart=%d dstart=%d\n",
		       file, dentry, fstart, dstart);
	}
	if (fend != dend) {
		PRINT_CALLER();
		printk(" CF2: file/dentry=%p:%p fend=%d dend=%d\n",
		       file, dentry, fend, dend);
	}
	inode = dentry->d_inode;
	if (!S_ISDIR(inode->i_mode)) {
		if (fend != fstart) {
			PRINT_CALLER();
			printk(" CF3: file/inode=%p:%p fstart=%d fend=%d\n",
			       file, inode, fstart, fend);
		}
		if (dend != dstart) {
			PRINT_CALLER();
			printk(" CF4: file/dentry=%p:%p dstart=%d dend=%d\n",
			       file, dentry, dstart, dend);
		}
	}

	/*
	 * check for NULL dentries inside the start/end range, or
	 * non-NULL dentries outside the start/end range.
	 */
	for (bindex = sbstart(sb); bindex < sbmax(sb); bindex++) {
		lower_file = unionfs_lower_file_idx(file, bindex);
		if (lower_file) {
			if (bindex < fstart || bindex > fend) {
				PRINT_CALLER();
				printk(" CF5: file/lower=%p:%p bindex=%d fstart/end=%d:%d\n",
				       file, lower_file, bindex, fstart, fend);
			}
		} else {	/* lower_file == NULL */
			if (bindex >= fstart && bindex <= fend) {
				/*
				 * directories can have NULL lower inodes in
				 * b/t start/end, but NOT if at the
				 * start/end range.
				 */
				if (!(S_ISDIR(inode->i_mode) &&
				      bindex > fstart && bindex < fend)) {
					PRINT_CALLER();
					printk(" CF6: file/lower=%p:%p "
					       "bindex=%d fstart/end=%d:%d\n",
					       file, lower_file, bindex,
					       fstart, fend);
				}
			}
		}
	}

	__unionfs_check_dentry(dentry,fname,fxn,line);
}

/* useful to track vfsmount leaks that could cause EBUSY on unmount */
void __show_branch_counts(const struct super_block *sb,
			  const char *file, const char *fxn, int line)
{
	int i;
	struct vfsmount *mnt;

	printk("BC:");
	for (i=0; i<sbmax(sb); i++) {
		mnt = UNIONFS_D(sb->s_root)->lower_paths[i].mnt;
		printk("%d:", (mnt ? atomic_read(&mnt->mnt_count) : -99));
	}
	printk("%s:%s:%d\n",file,fxn,line);
}
