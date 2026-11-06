/* SPDX-License-Identifier: MIT */
#ifndef QFUSE_CORE_H
#define QFUSE_CORE_H

#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 31
#endif
#include <fuse.h>

/* Internal API, separate from upstream fuse.h. */
struct qf_identity {
    uid_t uid;
    gid_t gid;
    pid_t pid;
    mode_t umask;
};

struct qf_session {
    struct fuse_operations ops;
    struct fuse_config config;
    struct fuse_context lifecycle_context;
    void *private_data;
    int ready;
    /* Backend-only scheduling hooks; never part of upstream fuse.h ABI. */
    int (*callback_pause)(unsigned *);
    void (*callback_resume)(unsigned);
};

/* A request is owned by one worker. Identity must come from the OS adapter. */
struct qf_request {
    struct qf_session *session;
    struct fuse_context context;
};

/* All operation results use FUSE's negative errno convention. */
int qf_session_start(struct qf_session *, const struct fuse_operations *, size_t, void *,
                     struct qf_identity);
int qf_session_start_owned(struct qf_session *, const struct fuse_operations *, size_t, void *,
                           struct qf_identity, struct fuse *);
/* Stop only after draining all requests; start/stop are not concurrent. */
void qf_session_stop(struct qf_session *);
int qf_request_init(struct qf_request *, struct qf_session *, struct qf_identity);
int qf_getattr(struct qf_request *, const char *, struct stat *, struct fuse_file_info *);
int qf_open(struct qf_request *, const char *, struct fuse_file_info *);
int qf_read(struct qf_request *, const char *, char *, size_t, off_t, struct fuse_file_info *);
int qf_readdir(struct qf_request *, const char *, void *, fuse_fill_dir_t, off_t,
               struct fuse_file_info *);
int qf_release(struct qf_request *, const char *, struct fuse_file_info *);
int qf_opendir(struct qf_request *, const char *, struct fuse_file_info *);
int qf_releasedir(struct qf_request *, const char *, struct fuse_file_info *);
int qf_create(struct qf_request *, const char *, mode_t, struct fuse_file_info *);
int qf_write(struct qf_request *, const char *, const char *, size_t, off_t,
             struct fuse_file_info *);
int qf_truncate(struct qf_request *, const char *, off_t, struct fuse_file_info *);
int qf_mkdir(struct qf_request *, const char *, mode_t);
int qf_unlink(struct qf_request *, const char *);
int qf_rmdir(struct qf_request *, const char *);
int qf_rename(struct qf_request *, const char *, const char *);
int qf_symlink(struct qf_request *, const char *, const char *);
int qf_readlink(struct qf_request *, const char *, char *, size_t);
int qf_chmod(struct qf_request *, const char *, mode_t, struct fuse_file_info *);
int qf_chown(struct qf_request *, const char *, uid_t, gid_t, struct fuse_file_info *);
int qf_utimens(struct qf_request *, const char *, const struct timespec[2],
               struct fuse_file_info *);
int qf_statfs(struct qf_request *, const char *, struct statvfs *);
int qf_fsync(struct qf_request *, const char *, int, struct fuse_file_info *);
int qf_flush(struct qf_request *, const char *, struct fuse_file_info *);

#endif
