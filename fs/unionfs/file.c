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

static ssize_t unionfs_read(struct file *file, char __user *buf,
			    size_t count, loff_t *ppos)
{
	int err;

	unionfs_read_lock(file->f_path.dentry->d_sb);
	if ((err = unionfs_file_revalidate(file, 0)))
		goto out;
	unionfs_check_file(file);

	err = do_sync_read(file, buf, count, ppos);

	if (err >= 0)
		touch_atime(unionfs_lower_mnt(file->f_path.dentry),
			    unionfs_lower_dentry(file->f_path.dentry));

out:
	unionfs_read_unlock(file->f_path.dentry->d_sb);
	unionfs_check_file(file);
	return err;
}

static ssize_t unionfs_aio_read(struct kiocb *iocb, const struct iovec *iov,
				unsigned long nr_segs, loff_t pos)
{
	int err = 0;
	struct file *file = iocb->ki_filp;

	unionfs_read_lock(file->f_path.dentry->d_sb);
	if ((err = unionfs_file_revalidate(file, 0)))
		goto out;
	unionfs_check_file(file);

	err = generic_file_aio_read(iocb, iov, nr_segs, pos);

	if (err == -EIOCBQUEUED)
		err = wait_on_sync_kiocb(iocb);

	if (err >= 0)
		touch_atime(unionfs_lower_mnt(file->f_path.dentry),
			    unionfs_lower_dentry(file->f_path.dentry));

out:
	unionfs_read_unlock(file->f_path.dentry->d_sb);
	unionfs_check_file(file);
	return err;
}

static ssize_t unionfs_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	int err = 0;

	unionfs_read_lock(file->f_path.dentry->d_sb);
	if ((err = unionfs_file_revalidate(file, 1)))
		goto out;
	unionfs_check_file(file);

	err = do_sync_write(file, buf, count, ppos);
	/* update our inode times upon a successful lower write */
	if (err >= 0) {
		unionfs_copy_attr_times(file->f_path.dentry->d_inode);
		unionfs_check_file(file);
	}

out:
	unionfs_read_unlock(file->f_path.dentry->d_sb);
	return err;
}

static int unionfs_file_readdir(struct file *file, void *dirent,
				filldir_t filldir)
{
	return -ENOTDIR;
}

static int unionfs_mmap(struct file *file, struct vm_area_struct *vma)
{
	int err = 0;
	int willwrite;
	struct file *lower_file;

	unionfs_read_lock(file->f_path.dentry->d_sb);

	/* This might be deferred to mmap's writepage */
	willwrite = ((vma->vm_flags | VM_SHARED | VM_WRITE) == vma->vm_flags);
	if ((err = unionfs_file_revalidate(file, willwrite)))
		goto out;
	unionfs_check_file(file);

	/*
	 * File systems which do not implement ->writepage may use
	 * generic_file_readonly_mmap as their ->mmap op.  If you call
	 * generic_file_readonly_mmap with VM_WRITE, you'd get an -EINVAL.
	 * But we cannot call the lower ->mmap op, so we can't tell that
	 * writeable mappings won't work.  Therefore, our only choice is to
	 * check if the lower file system supports the ->writepage, and if
	 * not, return EINVAL (the same error that
	 * generic_file_readonly_mmap returns in that case).
	 */
	lower_file = unionfs_lower_file(file);
	if (willwrite && !lower_file->f_mapping->a_ops->writepage) {
		err = -EINVAL;
		printk("unionfs: branch %d file system does not support "
		       "writeable mmap\n", fbstart(file));
	} else {
		err = generic_file_mmap(file, vma);
		if (err)
			printk("unionfs: generic_file_mmap failed %d\n", err);
	}

out:
	unionfs_read_unlock(file->f_path.dentry->d_sb);
	if (!err) {
		/* copyup could cause parent dir times to change */
		unionfs_copy_attr_times(file->f_path.dentry->d_parent->d_inode);
		unionfs_check_file(file);
		unionfs_check_dentry(file->f_path.dentry->d_parent);
	}
	return err;
}

struct file_operations unionfs_main_fops = {
	.llseek		= generic_file_llseek,
	.read		= unionfs_read,
	.aio_read       = unionfs_aio_read,
	.write		= unionfs_write,
	.aio_write      = generic_file_aio_write,
	.readdir	= unionfs_file_readdir,
	.unlocked_ioctl	= unionfs_ioctl,
	.mmap		= unionfs_mmap,
	.open		= unionfs_open,
	.flush		= unionfs_flush,
	.release	= unionfs_file_release,
	.fsync		= file_fsync,
	.sendfile	= generic_file_sendfile,
};
