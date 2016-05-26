#include "undofs_fops.h"
#include "undofs_util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

/** Get file attributes.
 *
 * Similar to stat().  The 'st_dev' and 'st_blksize' fields are
 * ignored.  The 'st_ino' field is ignored except if the 'use_ino'
 * mount option is given.
 */
static int undofs_getattr(const char *path, struct stat *statbuf)
{
    LOG("getattr(%s, %p)", path, statbuf);

    int retstat = 0, retval = 0;
    char fpath[PATH_MAX];

    retstat = undofs_versiondir_path(fpath, path);
    if(retstat)
        return -errno;
    
    if(! is_directory(fpath))
    {
        if(undofs_latest_path(fpath, path))
            return -errno;
    }
    
    if(is_deleted(fpath))
        return -ENOENT;

    retstat = lstat(fpath, statbuf);
    if (retstat != 0)
    {
        retval = -errno;
        LOG_ERROR("lstat for %s failed (%d)", fpath, retstat);
    }

    return retval;
}

/** Read the target of a symbolic link
 *
 * The buffer should be filled with a null terminated string.  The
 * buffer size argument includes the space for the terminating
 * null character.  If the linkname is too long to fit in the
 * buffer, it should be truncated.  The return value should be 0
 * for success.
 */
static int undofs_readlink(const char *path, char *link, size_t size)
{
    LOG("readlink(%s, %p, %zu)", path, link, size);
    int retstat = 0, retval = 0;
    char fpath[PATH_MAX]; // ??/

    if(undofs_latest_path(fpath, path))
        return -errno;

    retstat = readlink(fpath, link, size - 1); // System readlink() doesn't have the trailing \0
    if (retstat < 0)
    {
        retval = -errno;
        LOG_ERROR("readlink of %s failed (%d)", path, retstat);
    } else  {
        link[retstat] = '\0'; // System readlink() doesn't have the trailing \0
    }

    return retval;
}

/** Create a file node
 *
 * [If?] there is no create() operation, mknod() will be called for
 * creation of all non-directory, non-symlink nodes.
 */
static int undofs_mknod(const char *path, mode_t mode, dev_t dev)
{
    LOG("mknod(%s, %x, %lx)", path, mode, dev);

    int retstat = 0, retval = 0;
    char fpath[PATH_MAX];

    if(undofs_new_path(fpath, path) != 0)
    {
        return -errno;
    }

    // On Linux this could just be 'mknod(path, mode, rdev)'
    if (S_ISREG(mode)) {
        retstat = open(fpath, O_CREAT | O_EXCL | O_WRONLY, mode);
        if (retstat < 0) {
            retval = -errno;
            LOG_ERROR("Failed to create regular file at %s (open returned %d)", fpath, retstat);
        } else {
            retstat = close(retstat);
            if (retstat < 0) {
                retval = -errno;
                LOG_ERROR("Failed to create regular file at %s (close returned %d)", fpath, retstat);
            }
        }
    } else
        if(S_ISFIFO(mode)) {
            retstat = mkfifo(fpath, mode);
            if (retstat < 0)
            {
                retval = -errno;
                LOG_ERROR("Failed to create FIFO node at %s (mkfifo returned %d)", fpath, retstat);
            }
        } else {
            retstat = mknod(fpath, mode, dev);
            if (retstat < 0)
            {
                retval = -errno;
                LOG_ERROR("Failed to create special node at %s (mknod returned %d)", fpath, retstat);
            }
        }

    return retval;
}

/** Create a directory */
static int undofs_mkdir(const char *path, mode_t mode)
{
    LOG("mkdir(%s, %x)", path, mode);

    int retstat = 0, retval = 0;
    char fpath[PATH_MAX];

    retstat = undofs_versiondir_path(fpath, path);
    if(retstat)
        return -errno;

    if(is_deleted(fpath))
    {
        if(undelete(fpath) < 0)
        {
            retval = -errno;
            LOG_ERROR("Failed to undelete directory %s.", fpath);
        }
    } else {
        retstat = mkdir(fpath, mode);
        if (retstat < 0)
        {
            retval = -errno;
            LOG_ERROR("Could not create the directory at %s.", fpath);
        } else {
            char dmarker[PATH_MAX];
            snprintf(dmarker, PATH_MAX, "%s/dir", fpath);
            if(touch(dmarker) != 0)
            {
                retval = -errno;
                LOG_ERROR("Could not create directory marker at %s.", dmarker);
            }
            rmdir(fpath);
        }
    }

    return retval;
}

/** Remove a file */
static int undofs_unlink(const char *path)
{
    LOG("unlink(%s)", path);

    int retstat = 0;
    char fpath[PATH_MAX];

    retstat = undofs_versiondir_path(fpath, path);
    if(retstat)
        return -errno;

    if(is_directory(fpath))
    {
        LOG("Cannot unlink %s, is a directory.", fpath);
        return -EISDIR;
    }

    if(is_deleted(fpath))
    {
        LOG("Already deleted %s, raising ENOENT.", fpath);
        return -ENOENT;
    } else {
        strncat(fpath, "/deleted", PATH_MAX);
        if(touch(fpath))
        {
            retstat = -EIO;
            LOG_ERROR("Failed to create deleted marker %s.", fpath);
        }
    }

    return retstat;
}

/** Remove a directory */
static int undofs_rmdir(const char *path)
{
    LOG("rmdir(%s)", path);

    int retstat = 0;
    char fpath[PATH_MAX];

    retstat = undofs_versiondir_path(fpath, path);
    if(retstat)
        return retstat;

    // TODO: check if all children are deleted!

    strncat(fpath, "/deleted", PATH_MAX);
    if(touch(fpath))
    {
        retstat = -EIO;
        LOG_ERROR("Failed to create deleted marker %s.", fpath);
    }

    return retstat;
}

/** Create a symbolic link */
// The parameters here are a little bit confusing, but do correspond
// to the symlink() system call.  The 'path' is where the link points,
// while the 'link' is the link itself.  So we need to leave the path
// unaltered, but insert the link into the mounted directory.
static int undofs_symlink(const char *path, const char *link)
{
    LOG("symlink(%s, %s)", path, link);
    int retstat = 0, retval = 0;
    char flink[PATH_MAX];

    if(undofs_new_path(flink, link) != 0)
        return -errno;

    retstat = symlink(path, flink);
    if (retstat < 0)
    {
        retval = -errno;
        LOG_ERROR("Failed to create symlink for %s (symlink returned %d)", flink, retstat);
    }

    return retval;
}

/** Rename a file */
// both path and newpath are fs-relative
static int undofs_rename(const char *path, const char *newpath)
{
    LOG("rename(%s, %s)", path, newpath);

    // TODO: copy file, mark as deleted, or just rename directory.
    // Or rename the whole thing and lose history of newpath.

    int retstat = 0, retval = 0;
    char fpath[PATH_MAX];
    char fnewpath[PATH_MAX];

    retstat = undofs_versiondir_path(fpath, path);
    if(retstat)
        return retstat;
    retstat = undofs_versiondir_path(fnewpath, newpath);
    if(retstat)
        return retstat;

    if(is_directory(fpath)) // Directory:
    {
        if(access(fnewpath, F_OK))
            LOG("Warning: moving directory to %s, but destination already exists and will be overwritten, deleting all history.", fnewpath);

        retstat = rename(fpath, fnewpath);
        if (retstat < 0)
        {
            retval = -errno;
            LOG_ERROR("rename of %s to %s failed (returned %d).", fpath, fnewpath, retstat);
        }
    } else {
        // Normal file: unlink (mark as deleted) the source, then copy the latest version to the new
        if(undofs_latest_path(fpath, path))
            return -errno;
        if(undofs_new_path(fnewpath, newpath))
            return -errno;

        retval = undofs_unlink(path);
        if(retval == 0)
        {
            if(clone_file(fpath, fnewpath))
            {
                retval = -errno;
                undelete(fpath);
            }
        }
    }

    return retval;
}

/** Create a hard link to a file */
static int undofs_link(const char *path, const char *newpath)
{
    LOG("link(%s, %s)", path, newpath);
    int retstat = 0, retval = 0;
    char fpath[PATH_MAX], fnewpath[PATH_MAX];

    retstat = undofs_versiondir_path(fpath, path);
    if(retstat)
        return retstat;
    
    if(is_directory(fpath))
    {
        LOG("Can't link to %s, is a directory", path);
        return -EISDIR;
    }


    if(undofs_latest_path(fpath, path))
        return -errno;
    if(undofs_new_path(fnewpath, newpath))
        return -errno;

    retstat = link(fpath, fnewpath);
    if (retstat < 0)
    {
        retval = -errno;
        LOG("Failed to link %s to %s (link returned %d)", path, newpath, retstat);
    }

    return retval;
}

/** Change the permission bits of a file */
static int undofs_chmod(const char *path, mode_t mode)
{
    LOG("chmod(%s, %x)", path, mode);
    int retstat = 0, retval = 0;
    char fpath[PATH_MAX];

    retstat = undofs_versiondir_path(fpath, path);
    if(retstat)
        return retstat;
    
    if(! is_directory(fpath))
    {
        if(undofs_latest_path(fpath, path))
            return -errno;
    }

    retstat = chmod(fpath, mode);
    if (retstat < 0)
    {
        retval = -errno;
        LOG_ERROR("Failed to change permissions for %s to %x (chmod returned %d)", fpath, mode, retstat);
    }

    return retval;
}

/** Change the owner and group of a file */
static int undofs_chown(const char *path, uid_t uid, gid_t gid)
{
    LOG("chown(%s, %x, %x)", path, uid, gid);
    int retstat = 0, retval = 0;
    char fpath[PATH_MAX];

    retstat = undofs_versiondir_path(fpath, path);
    if(retstat)
        return retstat;
    
    if(! is_directory(fpath))
    {
        if(undofs_latest_path(fpath, path))
            return -errno;
    }

    retstat = chown(fpath, uid, gid);
    if (retstat < 0)
    {
        retval = -errno;
        LOG_ERROR("Failed to chown %s (return value %d)", fpath, retstat);
    }

    return retval;
}

/** Change the size of a file */
static int undofs_truncate(const char *path, off_t newsize)
{
    LOG("truncate(%s, %ld)", path, newsize);
    int retstat = 0, retval = 0;
    char fpath[PATH_MAX];

    if(undofs_latest_path(fpath, path))
        return -errno;

    retstat = truncate(fpath, newsize);
    if (retstat < 0)
    {
        retval = -errno;
        LOG_ERROR("truncate of %s failed (return value %d)", fpath, retstat);
    }

    return retval;
}

/** Change the access and/or modification times of a file */
/* note -- I'll want to change this as soon as 2.6 is in debian testing */
static int undofs_utime(const char *path, struct utimbuf *ubuf)
{
    LOG("utime(%s, %p)", path, ubuf);
    int retstat = 0, retval = 0;
    char fpath[PATH_MAX];

    if(undofs_latest_path(fpath, path))
        return -errno;

    retstat = utime(fpath, ubuf);
    if (retstat < 0)
    {
        retval = -errno;
        LOG_ERROR("Failed to change timestamps of %s (utime returned %d)", fpath, retstat);
    }

    return retval;
}

/** File open operation
 *
 * No creation, or truncation flags (O_CREAT, O_EXCL, O_TRUNC)
 * will be passed to open().  Open should check if the operation
 * is permitted for the given flags.  Optionally open may also
 * return an arbitrary filehandle in the fuse_file_info structure,
 * which will be passed to all file operations.
 *
 * Changed in version 2.2
 */
static int undofs_open(const char *path, struct fuse_file_info *fi)
{
    LOG("open(%s, %x)", path, fi->flags);
    int retval = 0;
    int fd;
    char fpath[PATH_MAX];

    if(fi->flags & O_RDWR || fi->flags & O_WRONLY)
    {
        undofs_new_path(fpath, path);
    } else {
        if(undofs_latest_path(fpath, path))
            return -errno;
    }

    LOG("Opening %s", fpath);

    fd = open(fpath, fi->flags);
    if (fd < 0)
    {
        retval = -errno;
        LOG_ERROR("open of %s failed (returned %d)", fpath, fd);
    } else {
        LOG("Opened %s, file handle is %d", path, fd);
    }

    fi->fh = fd;

    return retval;
}

/** Read data from an open file
 *
 * Read should return exactly the number of bytes requested except
 * on EOF or error, otherwise the rest of the data will be
 * substituted with zeroes.  An exception to this is when the
 * 'direct_io' mount option is specified, in which case the return
 * value of the read system call will reflect the return value of
 * this operation.
 *
 * Changed in version 2.2
 */
// I don't fully understand the documentation above -- it doesn't
// match the documentation for the read() system call which says it
// can return with anything up to the amount of data requested. nor
// with the fusexmp code which returns the amount of data also
// returned by read.
static int undofs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    //LOG("read(%s, %p, %zd, %ld), file handle is %ld", path, buf, size, offset, fi->fh);
    int retstat = 0, retval = 0;

    retstat = pread(fi->fh, buf, size, offset);
    if (retstat < 0)
    {
        retval = -errno;
        LOG_ERROR("Failed to read(%s, %lu, %ld), fh = %lu, pread returned %d", path, size, offset, fi->fh, retstat);
    }
    retval = retstat;

    return retval;
}

/** Write data to an open file
 *
 * Write should return exactly the number of bytes requested
 * except on error.  An exception to this is when the 'direct_io'
 * mount option is specified (see read operation).
 *
 * Changed in version 2.2
 */
// As  with read(), the documentation above is inconsistent with the
// documentation for the write() system call.
static int undofs_write(const char *path, const char *buf, size_t size, off_t offset,
                 struct fuse_file_info *fi)
{
    //LOG("write(%s, %p, %zd, %ld), file handle is %ld", path, buf, size, offset, fi->fh);
    int retstat = 0, retval = 0;

    retstat = pwrite(fi->fh, buf, size, offset);
    if (retstat < 0)
    {
        retval = -errno;
        LOG_ERROR("Failed to write(%s, %lu, %ld), fh = %lu, pwrite returned %d", path, size, offset, fi->fh, retstat);
    }
    retval = retstat;

    return retval;
}

/** Get file system statistics
 *
 * The 'f_frsize', 'f_favail', 'f_fsid' and 'f_flag' fields are ignored
 *
 * Replaced 'struct statfs' parameter with 'struct statvfs' in
 * version 2.5
 */
static int undofs_statfs(const char *path, struct statvfs *statv)
{
    LOG("statfs(%s)", path);
    int retstat = 0, retval = 0;
    char fpath[PATH_MAX];

    retstat = undofs_versiondir_path(fpath, path);
    if(retstat)
        return retstat;
    
    if(! is_directory(fpath))
    {
        if(undofs_latest_path(fpath, path))
            return -errno;
    }

    // get stats for underlying filesystem
    retstat = statvfs(fpath, statv);
    if (retstat < 0)
    {
        retval = -errno;
        LOG_ERROR("Failed to get statistics for %s (statvfs returned %d)", fpath, retstat);
    }

    return retstat;
}

/** Possibly flush cached data
 *
 * BIG NOTE: This is not equivalent to fsync().  It's not a
 * request to sync dirty data.
 *
 * Flush is called on each close() of a file descriptor.  So if a
 * filesystem wants to return write errors in close() and the file
 * has cached dirty data, this is a good place to write back data
 * and return any errors.  Since many applications ignore close()
 * errors this is not always useful.
 *
 * NOTE: The flush() method may be called more than once for each
 * open().  This happens if more than one file descriptor refers
 * to an opened file due to dup(), dup2() or fork() calls.  It is
 * not possible to determine if a flush is final, so each flush
 * should be treated equally.  Multiple write-flush sequences are
 * relatively rare, so this shouldn't be a problem.
 *
 * Filesystems shouldn't assume that flush will always be called
 * after some writes, or that if will be called at all.
 *
 * Changed in version 2.2
 */
static int undofs_flush(const char *path, struct fuse_file_info *fi)
{
    LOG("flush(%s)", path);
    return 0;
}

/** Release an open file
 *
 * Release is called when there are no more references to an open
 * file: all file descriptors are closed and all memory mappings
 * are unmapped.
 *
 * For every open() call there will be exactly one release() call
 * with the same flags and file descriptor.  It is possible to
 * have a file opened more than once, in which case only the last
 * release will mean, that no more reads/writes will happen on the
 * file.  The return value of release is ignored.
 *
 * Changed in version 2.2
 */
static int undofs_release(const char *path, struct fuse_file_info *fi)
{
    LOG("close(%s), file handle is %lu", path, fi->fh);
    int retstat = 0, retval = 0;

    // We need to close the file.  Had we allocated any resources
    // (buffers etc) we'd need to free them here as well.
    retstat = close(fi->fh);
    if(retstat < 0)
    {
        retval = -errno;
        LOG_ERROR("Release failed (close returned %d)", retstat);
    }

    return retval;
}

/** Synchronize file contents
 *
 * If the datasync parameter is non-zero, then only the user data
 * should be flushed, not the meta data.
 *
 * Changed in version 2.2
 */
static int undofs_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
    LOG("fsync(%s, %d), file handle is %lu", path, datasync, fi->fh);
    int retstat = 0, retval = 0;

    // some unix-like systems (notably freebsd) don't have a datasync call
#ifdef HAVE_FDATASYNC
    if (datasync)
        retstat = fdatasync(fi->fh);
    else
#endif
        retstat = fsync(fi->fh);

    if (retstat < 0)
    {
        retval = -errno;
        LOG_ERROR("Failed to fsync(%lu), return value is %d", fi->fh, retstat);
    }

    return retval;
}

/** Open directory
 *
 * This method should check if the open operation is permitted for
 * this  directory
 *
 * Introduced in version 2.3
 */
static int undofs_opendir(const char *path, struct fuse_file_info *fi)
{
    LOG("opendir(%s)", path);
    DIR *dp;
    int retstat = 0;
    char fpath[PATH_MAX];

    retstat = undofs_versiondir_path(fpath, path);
    if(retstat)
        return retstat;
    
    if(! is_directory(fpath))
    {
        LOG("Tried to open %s as a directory, but it's not a directory.", fpath);
        return -ENOTDIR;
    }

    dp = opendir(fpath);
    if (dp == NULL)
    {
        retstat = -errno;
        LOG_ERROR("Failed to open the directory %s", fpath);
    }

    fi->fh = (intptr_t) dp;

    return retstat;
}

/** Read directory
 *
 * This supersedes the old getdir() interface.  New applications
 * should use this.
 *
 * The filesystem may choose between two modes of operation:
 *
 * 1) The readdir implementation ignores the offset parameter, and
 * passes zero to the filler function's offset.  The filler
 * function will not return '1' (unless an error happens), so the
 * whole directory is read in a single readdir operation.  This
 * works just like the old getdir() method.
 *
 * 2) The readdir implementation keeps track of the offsets of the
 * directory entries.  It uses the offset parameter and always
 * passes non-zero offset to the filler function.  When the buffer
 * is full (or an error happens) the filler function will return
 * '1'.
 *
 * Introduced in version 2.3
 */
static int undofs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
                   struct fuse_file_info *fi)
{
    LOG("readdir(%s), offset %ld", path, offset);
    DIR *dp;
    struct dirent *de;
    char dirpath[PATH_MAX];
    if(undofs_versiondir_path(dirpath, path))
        return -errno;

    // once again, no need for fullpath -- but note that I need to cast fi->fh
    dp = (DIR *) (uintptr_t) fi->fh;

    // Every directory contains at least two entries: . and ..  If my
    // first call to the system readdir() returns NULL I've got an
    // error; near as I can tell, that's the only condition under
    // which I can get an error from readdir()
    de = readdir(dp);
    if (de == 0) {
        int retval = -errno;
        LOG_ERROR("readdir call returned NULL");
        return retval;
    }

    // List the . and .. entries.
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    // This will copy the entire directory into the buffer.  The loop exits
    // when either the system readdir() returns NULL, or filler()
    // returns something non-zero.  The first case just means I've
    // read the whole directory; the second means the buffer is full.
    do {
        char rpath[PATH_MAX], fpath[PATH_MAX];
        int file_ok = 0, name_wrong = 0;
        snprintf(fpath, PATH_MAX, "%s/%s", dirpath, de->d_name);
        name_wrong |= undofs_clean_name(rpath, fpath);
        if(name_wrong)
            continue;
    
        if(is_directory(fpath))
        {
            if(!is_deleted(fpath))
                file_ok = 1;
        } else {
            undofs_latest_path(fpath, rpath);
            if(access(fpath, F_OK) == 0)
                file_ok = 1;
        }

        if(! file_ok)
        {
            LOG("While reading %s, %s seems to be neither an undofs directory nor file, skipping.", path, de->d_name);
            continue;
        }

        name_wrong |= undofs_clean_name(rpath, de->d_name);
        if(name_wrong)
            continue;

        if (filler(buf, rpath, NULL, 0) != 0) {
            errno = ENOMEM;
            LOG_ERROR("readdir filler callback failed, is the buffer full?");
            return -ENOMEM;
        }
    } while ((de = readdir(dp)) != NULL);

    return 0;
}

/** Release directory
 *
 * Introduced in version 2.3
 */
static int undofs_releasedir(const char *path, struct fuse_file_info *fi)
{
    LOG("releasedir(%s)", path);

    closedir((DIR *) (uintptr_t) fi->fh);

    return 0;
}

/** Synchronize directory contents
 *
 * If the datasync parameter is non-zero, then only the user data
 * should be flushed, not the meta data
 *
 * Introduced in version 2.3
 */
// when exactly is this called?  when a user calls fsync and it
// happens to be a directory? ???
static int undofs_fsyncdir(const char *path, int datasync, struct fuse_file_info *fi)
{
    LOG("fsyncdir(%s, %d)", path, datasync);
    return 0;
}

/**
 * Check file access permissions
 *
 * This will be called for the access() system call.  If the
 * 'default_permissions' mount option is given, this method is not
 * called.
 *
 * This method is not called under Linux kernel versions 2.4.x
 *
 * Introduced in version 2.5
 */
static int undofs_access(const char *path, int mask)
{
    LOG("access(%s, %x)", path, mask);
    int retstat = 0, retval = 0;
    char fpath[PATH_MAX];

    retstat = undofs_versiondir_path(fpath, path);
    if(retstat)
        return retstat;

    if(! is_directory(fpath))
    {
        if(undofs_latest_path(fpath, path))
            return -errno;
    }

    retstat = access(fpath, mask);

    if (retstat < 0)
    {
        retval = -errno;
        LOG_ERROR("Failed to determine permissions of %s (access returned %d)", fpath, retstat);
    }

    return retval;
}

/**
 * Create and open a file
 *
 * If the file does not exist, first create it with the specified
 * mode, and then open it.
 *
 * If this method is not implemented or under Linux kernel
 * versions earlier than 2.6.15, the mknod() and open() methods
 * will be called instead.
 *
 * Introduced in version 2.5
 */
static int undofs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    LOG("create(%s, %x)", path, mode);
    int retstat = 0;
    char fpath[PATH_MAX];
    int fd;

    if(undofs_new_path(fpath, path))
        return -errno;

    fd = creat(fpath, mode);
    if (fd < 0)
    {
        retstat = -errno;
        LOG_ERROR("Failed to create file %s, returned handle was %d", fpath, fd);
    }

    fi->fh = fd;

    return retstat;
}

/**
 * Change the size of an open file
 *
 * This method is called instead of the truncate() method if the
 * truncation was invoked from an ftruncate() system call.
 *
 * If this method is not implemented or under Linux kernel
 * versions earlier than 2.6.15, the truncate() method will be
 * called instead.
 *
 * Introduced in version 2.5
 */
static int undofs_ftruncate(const char *path, off_t offset, struct fuse_file_info *fi)
{
    LOG("ftruncate(%s, %ld), file handle is %lu.", path, offset, fi->fh);
    int retstat = 0;

    retstat = ftruncate(fi->fh, offset);
    if (retstat < 0)
    {
        int retval = -errno;
        LOG_ERROR("Failed to truncate %s (%lu), ftruncate returned %d", path, fi->fh, retstat);
        return retval;
    }

    return retstat;
}

/**
 * Get attributes from an open file
 *
 * This method is called instead of the getattr() method if the
 * file information is available.
 *
 * Currently this is only called after the create() method if that
 * is implemented (see above).  Later it may be called for
 * invocations of fstat() too.
 *
 * Introduced in version 2.5
 */
static int undofs_fgetattr(const char *path, struct stat *statbuf, struct fuse_file_info *fi)
{
    LOG("fgetattr(%s), file handle is %ld", path, fi->fh);
    int retstat = 0, retval = 0;

    // On FreeBSD, trying to do anything with the mountpoint ends up
    // opening it, and then using the FD for an fgetattr.  So in the
    // special case of a path of "/", I need to do a getattr on the
    // underlying root directory instead of doing the fgetattr().
    if (!strcmp(path, "/"))
        return undofs_getattr(path, statbuf);

    retstat = fstat(fi->fh, statbuf);
    if (retstat < 0)
    {
        retval = -errno;
        LOG_ERROR("fstat failed for %s (%lu), return value was %d", path, fi->fh, retstat);
    }

    return retval;
}

/**
 * Initialize filesystem
 *
 * The return value will passed in the private_data field of
 * fuse_context to all file operations and as a parameter to the
 * destroy() method.
 *
 * Introduced in version 2.3
 * Changed in version 2.6
 */
// Undocumented but extraordinarily useful fact:  the fuse_context is
// set up before this function is called, and
// fuse_get_context()->private_data returns the user_data passed to
// fuse_main().  Really seems like either it should be a third
// parameter coming in here, or else the fact should be documented
// (and this might as well return void, as it did in older versions of
// FUSE).
static void *undofs_init(struct fuse_conn_info *conn)
{
    LOG("Init undofs.");

    return fuse_get_context()->private_data;
}

/**
 * Clean up filesystem
 *
 * Called on filesystem exit.
 *
 * Introduced in version 2.3
 */
static void undofs_destroy(void *userdata)
{
    LOG("Destroying undofs");
}

struct fuse_operations undofs_oper = {
    .getattr = undofs_getattr,
    .readlink = undofs_readlink,
    // no .getdir -- that's deprecated
    .getdir = NULL,
    .mknod = undofs_mknod,
    .mkdir = undofs_mkdir,
    .unlink = undofs_unlink,
    .rmdir = undofs_rmdir,
    .symlink = undofs_symlink,
    .rename = undofs_rename,
    .link = undofs_link,
    .chmod = undofs_chmod,
    .chown = undofs_chown,
    .truncate = undofs_truncate,
    .utime = undofs_utime,
    .open = undofs_open,
    .read = undofs_read,
    .write = undofs_write,
    .statfs = undofs_statfs,
    .flush = undofs_flush,
    .release = undofs_release,
    .fsync = undofs_fsync,
    .opendir = undofs_opendir,
    .readdir = undofs_readdir,
    .releasedir = undofs_releasedir,
    .fsyncdir = undofs_fsyncdir,
    .init = undofs_init,
    .destroy = undofs_destroy,
    .access = undofs_access,
    .create = undofs_create,
    .ftruncate = undofs_ftruncate,
    .fgetattr = undofs_fgetattr
};

struct fuse_operations *undofs_operations()
{
    return &undofs_oper;
}
