#include "undofs_util.h"

#include <dirent.h>
#include <fuse.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    const char* rootdir;
} undofs_state;

#define PRIVATE_DATA ((undofs_state *) fuse_get_context()->private_data)

static FILE* logf = NULL;
FILE* undofs_logfile()
{
    if(logf == NULL)
    {
        char logpath[PATH_MAX];
        snprintf(logpath, PATH_MAX, "%s/log.txt", PRIVATE_DATA->rootdir);
        logf = fopen(logpath, "a");
        LOG("Opened log file.");
    }
    return logf;
}

void* create_private_data(const char* rootdir)
{
    undofs_state* context = calloc(sizeof(undofs_state), 1);
    context->rootdir = rootdir;
    return context;
}

int undofs_versiondir_path(char* fpath, const char *path)
{
    if(strcmp(path, "/") == 0)
    {
        snprintf(fpath, PATH_MAX, "%s", PRIVATE_DATA->rootdir);
        return 0;
    }
    else
    {
        int i = 0;
        const char* path_pos = path;
        char * fpath_pos = fpath;
        i = snprintf(fpath, PATH_MAX, "%s", PRIVATE_DATA->rootdir);
        fpath_pos += i;

        // Ignore leading '/' for mangling.
        while(*path_pos == '/')
        {
            *fpath_pos++ = *path_pos++;
            i++;
        }
        
        // 12 is reserved for inserting a .node/ + final .node\0
        while(fpath_pos + 12 < fpath + PATH_MAX
              && *path_pos)
        {
            if(*path_pos == '/')
            {
                *fpath_pos++ = '.';
                *fpath_pos++ = 'n';
                *fpath_pos++ = 'o';
                *fpath_pos++ = 'd';
                *fpath_pos++ = 'e';
                // Skip double slashes in filenames.
                while(path_pos[1] == '/') path_pos++;
            }
            *fpath_pos++ = *path_pos++;
        }

        if (*path_pos)
        {
            *fpath_pos = 0;
            errno = ENAMETOOLONG;
            LOG_ERROR("Ran out of space before finishing mangling %s, result so far: %s", path, fpath);
            return -1;
        }
        
        // Trailing .node need not be added for root directory.
        if(fpath_pos - fpath > i)
        {
            *fpath_pos++ = '.';
            *fpath_pos++ = 'n';
            *fpath_pos++ = 'o';
            *fpath_pos++ = 'd';
            *fpath_pos++ = 'e';
        }
        *fpath_pos++ = '\0';

        LOG("Mangle '%s' -> '%s'.", path, fpath);
        return 0;
    }
    
}

long undofs_latest_version(const char *path)
{
    char fpath[PATH_MAX];
    long max_file = -1;

    if(undofs_versiondir_path(fpath, path))
        return -1;

    DIR *dirp = opendir(fpath);

    if(dirp == NULL)
    {
        if(errno != ENOENT)
            LOG_ERROR("Failed to look up file version for %s", path);
    } else {
        struct dirent entry, *not_end = &entry;
        while(not_end)
        {
            if(readdir_r(dirp, &entry, &not_end) != 0)
            {
                max_file = -1;
                break;
            }
            long curr_file = strtol(entry.d_name,NULL,10);
            if(curr_file > max_file)
                max_file = curr_file;
        }
    }
    closedir(dirp);

    LOG("Latest version of %s is %ld", path, max_file);
    return max_file;
}

int undofs_latest_path(char *fpath, const char *path)
{
    long version = undofs_latest_version(path);
    char directory_path[PATH_MAX];
    if(undofs_versiondir_path(directory_path, path))
        return -1;

    if(is_deleted(directory_path))
        version++;
    if(is_directory(directory_path))
        snprintf(fpath, PATH_MAX, "%s", directory_path);
    else
        snprintf(fpath, PATH_MAX, "%s/%ld", directory_path, version);
    return 0;
}

int undofs_new_path(char fpath[PATH_MAX], const char *path)
{
    long version = undofs_latest_version(path);

    char directory_path[PATH_MAX], old_path[PATH_MAX];
    if(undofs_versiondir_path(directory_path, path))
        return -1;

    if(is_directory(directory_path))
    {
        LOG("Requested a new version of %s, but this is a directory.", path);
        errno = EISDIR;
        *fpath = 0;
        return -1;
    }

    snprintf(old_path, PATH_MAX, "%s/%ld", directory_path, version);
    snprintf(fpath, PATH_MAX, "%s/%ld", directory_path, version+1);
    LOG("Creating new version at %s", fpath);

    if(version >= 0)
    {
        int deleted = is_deleted(directory_path);

        if(deleted)
            undelete(directory_path);

        if(!deleted)
        {
            if(clone_file(old_path, fpath) != 0)
            {
                LOG_ERROR("Failed to create a new version of '%s'", path);
                return -1;
            }
        }
    } else {
        int res = mkdir(directory_path, S_IRWXU);
        if(res != 0)
        {
            LOG_ERROR("Failed to create new directory for %s", directory_path);
            return -1;
        }
    }

    return 0;
}

int undofs_clean_name(char* name, const char *mangled)
{
    // TODO: ENAMETOOLONG
    char *name_pos = name;
    const char *mangled_pos = mangled;
    int demangled_last_char = 1, retval = 0;

    // Ignore leading rootdir
    size_t rootdir_len = strlen(PRIVATE_DATA->rootdir);
    if(strncmp(PRIVATE_DATA->rootdir, mangled, rootdir_len) == 0)
        mangled_pos += rootdir_len;
    
    // Ignore leading '/' for demangling.
    if(*mangled_pos == '/')
        *name_pos++ = *mangled_pos++;

    while(*mangled_pos)
    {
        // Skip .node at end of string or before path separator.
        if(strncmp(mangled_pos, ".node", 5) == 0 &&
           (mangled_pos[5] == '/' || mangled_pos[5] == '\0'))
        {
            demangled_last_char = 1;
            mangled_pos += 5;
        } else {
            if(*mangled_pos == '/' && demangled_last_char == 0)
            {
                retval = -1;
                *name_pos = 0; // Null-terminate for print
                LOG("Warning: filename '%s' is not fully mangled (at '%s').", mangled, name);
            }
            *name_pos++ = *mangled_pos++;
            demangled_last_char = 0;
        }
    }
    *name_pos = 0;

    if (demangled_last_char == 0)
    {
        retval = -1;
        LOG("Warning: filename '%s' is not fully mangled (at '%s').", mangled, name);
    }

    LOG("Demangle '%s' -> '%s'", mangled, name);
    return retval;
}

int is_directory(const char *path)
{
    char directory_path[PATH_MAX];
    snprintf(directory_path, PATH_MAX, "%s/dir", path);
    return (access(directory_path, F_OK) == 0);
}

int is_deleted(const char *path)
{
    char deleted_path[PATH_MAX];
    snprintf(deleted_path, PATH_MAX, "%s/deleted", path);
    return (access(deleted_path, F_OK) == 0);
}

int undelete(const char *path)
{
    char deleted_path[PATH_MAX];
    snprintf(deleted_path, PATH_MAX, "%s/deleted", path);
    return unlink(deleted_path);
}

int touch(const char *path)
{
    LOG("Touching %s", path);
    int retstat = open(path, O_CREAT | O_EXCL | O_WRONLY);
    if (retstat < 0)
        return -1;

    close(retstat);
    return 0;
}

int clone_file(const char *src, const char *dst)
{
    int childExitStatus;
    pid_t pid;

    pid = fork();

    if(pid == 0)
    {
        execl("/bin/cp", "/bin/cp", "-a", src, dst, (char *)0);
    }
    else if(pid < 0)
    {
        LOG_ERROR("Failed to fork process to clone %s to %s", src, dst);
        return -1;
    }
    else {
        pid_t ws = waitpid( pid, &childExitStatus, 0);
        if(ws == -1)
        {
            LOG_ERROR("Failed to fork process to clone %s to %s", src, dst);
            return -1;
        }

        if(WIFEXITED(childExitStatus))
        {
            return 0;
        }
        else if(WIFSIGNALED(childExitStatus) || WIFSTOPPED(childExitStatus))
        {
            LOG_ERROR("cp exited abnormally while copying %s to %s", src, dst);
            return -1;
        }
    }
    LOG_ERROR("Reached end of clone_file while copying %s to %s", src, dst);
    return -1;
}
