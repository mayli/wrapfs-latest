obj-$(CONFIG_UNION_FS) += unionfs.o

unionfs-y := subr.o dentry.o file.o inode.o main.o super.o \
	rdstate.o copyup.o dirhelper.o rename.o \
	unlink.o lookup.o commonfops.o dirfops.o sioq.o \
	mmap.o

unionfs-$(CONFIG_UNION_FS_XATTR) += xattr.o

# If you want debugging output, please uncomment the following line
# or put your options in a separate file in linux-x.y.z/fs/unionfs/local.mk
#CONFIG_UNIONFS_DEBUG=y

# Allow users to override debug options in a separate file
-include fs/unionfs/local.mk

ifeq ($(CONFIG_UNIONFS_DEBUG),y)
unionfs-y += debug.o
EXTRA_CFLAGS += -DUNIONFS_DEBUG=1
endif
