obj-$(CONFIG_UNION_FS) += unionfs.o

unionfs-y := subr.o dentry.o file.o inode.o main.o super.o \
	rdstate.o copyup.o dirhelper.o rename.o \
	unlink.o lookup.o commonfops.o dirfops.o sioq.o

unionfs-$(CONFIG_UNION_FS_XATTR) += xattr.o

# If you want debugging output, please uncomment the following line
#CONFIG_UNIONFS_DEBUG=y

ifeq ($(CONFIG_UNIONFS_DEBUG),y)
unionfs-y += debug.o
EXTRA_CFLAGS += -DUNIONFS_DEBUG=1
endif
