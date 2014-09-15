#ifndef __UNDOFS_UTILS_H_
#define __UNDOFS_UTILS_H_
#include "config.h"

#include <sys/wait.h>
#include <limits.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#define LOG(fmt, ...) { fprintf(undofs_logfile(),  "%s:%d\t"  fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); fflush(undofs_logfile()); }
#define LOG_ERROR(fmt, ...) LOG("[error: %s] " fmt "\n", strerror(errno), ##__VA_ARGS__)

FILE* undofs_logfile();

/**
 * Allocate and initialize private data for undofs.
 * @param rootdir The root directory to use for data storage.
 * @return pointer to be used as private data by fuse.
 */
void* create_private_data(const char* rootdir);

/**
 * Convert a relative path of a directory to the directory in the mounted fs.
 * @param fpath Output parameter to store the directory path.
 * @param path String containing relative path of the requested directory.
 */
void undofs_directory_path(char fpath[PATH_MAX], const char *path);

/**
 * Convert a relative path to the absolute directory path containing the different revisions of a file.
 *
 * @param fpath Output parameter to store the directory path.
 * @param path String containing relative path of the requested file.
 */
void undofs_versiondir_path(char fpath[PATH_MAX], const char *path);

/**
 * Checks which is the latest version number of a given file.
 * @param path Relative path of the requested file.
 * @return the latest version number of a file, or -1 if no such file exists.
 */
long undofs_latest_version(const char *path);

/**
 * Convert a relative path to the absolute path of newest version of a file.
 * @param fpath container for the absolute file path.
 * @param path original relative path provided by FUSE.
 * @return 0 on success, -1 on failure.
 */
int undofs_latest_path(char fpath[PATH_MAX], const char *path);

/**
 * Convert an undofs mangled filename to a clean name.
 * @param name container for the clean filename.
 * @param mangled the mangled file name.
 */
void undofs_clean_name(char name[PATH_MAX], const char *mangled);

/**
 * Create a newer revision of a file.
 * @param fpath container for the absolute path to the new version.
 * @param path the relative path of the file, provided by FUSE.
 * @return return 0 on succes, or a negative number on error.
 */
int undofs_new_path(char fpath[PATH_MAX], const char *path);

/**
 * Check if a file or directory is marked as deleted.
 * A non-existent file is considered to not have been deleted.
 *
 * @param path The path to the file or directory, should be the output of undofs_directory_path or undofs_versiondir_path.
 * @return 0 if the file is not marked as deleted, non-zero if it is.
 */
int is_deleted(const char *path);

/**
 * Undelete a file.
 * @param path The path to the file or directory, should be the output of undofs_directory_path or undofs_versiondir_path.
 * @return 0 on success, negative number on failure.
 */
int undelete(const char *path);

/**
 * Create a normal file or update the timestamp on an existing file.
 * @param path Path to the target file (absolute, not an undofs path!)
 * @return 0 on success.
 */
int touch(const char *path);

/**
 * Clone a file, including all permissions and properties.
 *
 * In case of an error, errno will be set appropriately.
 *
 * @param src Source file to copy.
 * @param dst Destination to copy the cloned file to.
 * @return 0 when succesful, or a negative value in case of an error.
 */
int clone_file(const char *src, const char *dst);

/**
 * Check if a undofs node is a directory or not.
 * A non-existent node is not a directory.
 * @param path the node path (as returned by undofs_directory_path() or undofs_versiondir_path()
 * @return 1 if the node is a directory, 0 otherwise.
 */
int is_directory(const char *path);

#endif
