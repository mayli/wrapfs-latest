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

/*******************
 * File Operations *
 *******************/

static ssize_t unionfs_read(struct file *file, char __user *buf,
			    size_t count, loff_t *ppos)
{
	int err;

	unionfs_read_lock(file->f_dentry->d_sb);
	if ((err = unionfs_file_revalidate(file, 0)))
		goto out;

	err = do_sync_read(file, buf, count, ppos);

/*
	FIXME: do_sync_read updates a time
	if (err >= 0)
		touch_atime(unionfs_lower_mnt(file->f_path.dentry),
				unionfs_lower_dentry(file->f_path.dentry));
*/

out:
	unionfs_read_unlock(file->f_dentry->d_sb);
	unionfs_check_file(file);
	return err;
}

static ssize_t unionfs_aio_read(struct kiocb *iocb, const struct iovec *iov,
				unsigned long nr_segs, loff_t pos)
{
	struct file *file = iocb->ki_filp;
	int err;
#error fixme fxn check_file? read_unlock?
	err = generic_file_aio_read(iocb, iov, nr_segs, pos);

	if (err == -EIOCBQUEUED)
		err = wait_on_sync_kiocb(iocb);

/*	XXX: is this needed?
	if (err >= 0)
		touch_atime(unionfs_lower_mnt(file->f_path.dentry),
				unionfs_lower_dentry(file->f_path.dentry));
*/

#if 0
out:
	unionfs_read_unlock(file->f_dentry->d_sb);
	unionfs_check_file(file);
#endif
	return err;
}
static ssize_t unionfs_write(struct file * file, const char __user * buf,
			size_t count, loff_t * ppos)
{
	int err = 0;

	unionfs_read_lock(file->f_dentry->d_sb);
	if ((err = unionfs_file_revalidate(file, 1)))
		goto out;

	err = do_sync_write(file, buf, count, ppos);

out:
	unionfs_read_unlock(file->f_dentry->d_sb);
	unionfs_check_file(file);
	return err;
}

static int unionfs_file_readdir(struct file *file, void *dirent,
				filldir_t filldir)
{
	return -ENOTDIR;
}

static unsigned int unionfs_poll(struct file *file, poll_table *wait)
{
	unsigned int mask = DEFAULT_POLLMASK;
	struct file *hidden_file = NULL;

	unionfs_read_lock(file->f_dentry->d_sb);
	if (unionfs_file_revalidate(file, 0)) {
		/* We should pretend an error happened. */
		mask = POLLERR | POLLIN | POLLOUT;
		goto out;
	}

	hidden_file = unionfs_lower_file(file);

	if (!hidden_file->f_op || !hidden_file->f_op->poll)
		goto out;

	mask = hidden_file->f_op->poll(hidden_file, wait);

out:
	unionfs_read_unlock(file->f_dentry->d_sb);
	unionfs_check_file(file);
	return mask;
}

static int __do_mmap(struct file *file, struct vm_area_struct *vma)
{
	int err;
	struct file *hidden_file;

	hidden_file = unionfs_lower_file(file);

	err = -ENODEV;
	if (!hidden_file->f_op || !hidden_file->f_op->mmap)
		goto out;

	vma->vm_file = hidden_file;
	err = hidden_file->f_op->mmap(hidden_file, vma);
	get_file(hidden_file);	/* make sure it doesn't get freed on us */
	fput(file);		/* no need to keep extra ref on ours */
out:
	return err;
}

static int unionfs_mmap(struct file *file, struct vm_area_struct *vma)
{
	int err = 0;
	int willwrite;

	unionfs_read_lock(file->f_dentry->d_sb);
	/* This might could be deferred to mmap's writepage. */
	willwrite = ((vma->vm_flags | VM_SHARED | VM_WRITE) == vma->vm_flags);
	if ((err = unionfs_file_revalidate(file, willwrite)))
		goto out;

	err = __do_mmap(file, vma);

out:
	unionfs_read_unlock(file->f_dentry->d_sb);
	unionfs_check_file(file);
	return err;
}

static int unionfs_fsync(struct file *file, struct dentry *dentry,
			 int datasync)
{
	int err;
	struct file *hidden_file = NULL;

	unionfs_read_lock(file->f_dentry->d_sb);
	if ((err = unionfs_file_revalidate(file, 1)))
		goto out;

	hidden_file = unionfs_lower_file(file);

	err = -EINVAL;
	if (!hidden_file->f_op || !hidden_file->f_op->fsync)
		goto out;

	mutex_lock(&hidden_file->f_dentry->d_inode->i_mutex);
	err = hidden_file->f_op->fsync(hidden_file, hidden_file->f_dentry,
				       datasync);
	mutex_unlock(&hidden_file->f_dentry->d_inode->i_mutex);

out:
	unionfs_read_unlock(file->f_dentry->d_sb);
	unionfs_check_file(file);
	return err;
}

static int unionfs_fasync(int fd, struct file *file, int flag)
{
	int err = 0;
	struct file *hidden_file = NULL;

	unionfs_read_lock(file->f_dentry->d_sb);
	if ((err = unionfs_file_revalidate(file, 1)))
		goto out;

	hidden_file = unionfs_lower_file(file);

	if (hidden_file->f_op && hidden_file->f_op->fasync)
		err = hidden_file->f_op->fasync(fd, hidden_file, flag);

out:
	unionfs_read_unlock(file->f_dentry->d_sb);
	unionfs_check_file(file);
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
};
