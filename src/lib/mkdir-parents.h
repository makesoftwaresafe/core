#ifndef MKDIR_PARENTS_H
#define MKDIR_PARENTS_H

#include <sys/stat.h>

/* Returns the mode with executable bit added for each read/write bit. For
   example mode=0640 returns 0750. */
mode_t mkdir_get_executable_mode(mode_t mode);

/* Create path and all the directories under it if needed. Permissions for
   existing directories isn't changed. Returns 0 if ok. If directory already
   exists, returns -1 with errno=EEXIST. */
int mkdir_parents(const char *path, mode_t mode);

/* Like mkdir_parents(), but use the given uid/gid for newly created
   directories. (uid_t)-1 or (gid_t)-1 can be used to indicate that it
   doesn't need to be changed. If gid isn't (gid_t)-1 and the parent directory
   had setgid-bit enabled, it's removed unless explicitly included in the
   mode. */
int mkdir_parents_chown(const char *path, mode_t mode, uid_t uid, gid_t gid);
/* Like mkdir_parents_chown(), but change only group. If chown() fails with
   EACCES, use gid_origin in the error message. */
int mkdir_parents_chgrp(const char *path, mode_t mode,
			gid_t gid, const char *gid_origin);

/* Like mkdir_parents_chown(), but don't actually create any parents. */
int mkdir_chown(const char *path, mode_t mode, uid_t uid, gid_t gid);
int mkdir_chgrp(const char *path, mode_t mode,
		gid_t gid, const char *gid_origin);

/* stat() the path or its first parent that exists. Returns 0 if ok, -1 if
   failed. root_dir is set to the last stat()ed directory (on success and
   on failure). */
int stat_first_parent(const char *path, const char **root_dir_r,
		      struct stat *st_r);

#endif
