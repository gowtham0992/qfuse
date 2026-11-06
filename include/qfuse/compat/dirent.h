/* SPDX-License-Identifier: MIT
 * Opt-in source adapter for BSD/Linux d_type consumers on QNX 8.0.
 * Never pass these owned records or streams to native libc interfaces.
 */
#ifndef QFUSE_COMPAT_DIRENT_H
#define QFUSE_COMPAT_DIRENT_H
#ifndef __QNXNTO__
#error "QFuse's directory-entry adapter is only for QNX"
#endif
#include_next <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
#endif

struct qfuse_compat_dirent {
    ino_t d_ino;
    off_t d_off;
    unsigned short d_reclen;
    unsigned short d_namlen;
    unsigned char d_type;
    char d_name[NAME_MAX + 1];
};
typedef struct {
    DIR *native;
    struct qfuse_compat_dirent entry;
} qfuse_compat_DIR;

static inline qfuse_compat_DIR *qfuse_compat_opendir(const char *path)
{
    qfuse_compat_DIR *stream = calloc(1, sizeof(*stream));
    if (!stream)
        return NULL;
    stream->native = opendir(path);
    if (!stream->native) {
        int saved = errno;
        free(stream);
        errno = saved;
        return NULL;
    }
    int saved = errno;
    int flags = dircntl(stream->native, D_GETFLAG);
    if (flags != -1)
        (void)dircntl(stream->native, D_SETFLAG, flags | D_FLAG_STAT);
    errno = saved;
    return stream;
}
static inline struct qfuse_compat_dirent *qfuse_compat_readdir(qfuse_compat_DIR *stream)
{
    struct dirent *native = readdir(stream->native);
    if (!native)
        return NULL;
    int saved = errno;
    size_t length = strnlen(native->d_name, NAME_MAX + 1);
    if (length > NAME_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    struct qfuse_compat_dirent *entry = &stream->entry;
    memset(entry, 0, sizeof(*entry));
    entry->d_ino = native->d_ino;
    entry->d_off = native->d_offset;
    entry->d_namlen = (unsigned short)length;
    entry->d_reclen = (unsigned short)(offsetof(struct qfuse_compat_dirent, d_name) + length + 1);
    memcpy(entry->d_name, native->d_name, length + 1);
    /* Only native lstat extras can classify symlinks correctly. Never issue
     * path lookups here: a mirrored namespace may contain this server itself.
     * Missing/unsupported metadata remains the legitimate DT_UNKNOWN value. */
    for (struct dirent_extra *extra = _DEXTRA_FIRST(native);
         _DEXTRA_VALID(extra, native); extra = _DEXTRA_NEXT(extra)) {
        if (extra->d_type == _DTYPE_LSTAT && extra->d_datalen >= sizeof(struct stat)) {
            struct stat st;
            memcpy(&st, (const char *)extra + offsetof(struct dirent_extra_stat, d_stat), sizeof(st));
            entry->d_type = (unsigned char)((st.st_mode & S_IFMT) >> 12);
            break;
        }
    }
    errno = saved;
    return entry;
}
static inline int qfuse_compat_closedir(qfuse_compat_DIR *stream)
{
    int result = closedir(stream->native);
    int saved = errno;
    free(stream);
    errno = saved;
    return result;
}
static inline int qfuse_compat_dirfd(qfuse_compat_DIR *stream)
{
    return dirfd(stream->native);
}
static inline void qfuse_compat_rewinddir(qfuse_compat_DIR *stream)
{
    rewinddir(stream->native);
}
static inline long qfuse_compat_telldir(qfuse_compat_DIR *stream)
{
    return telldir(stream->native);
}
static inline void qfuse_compat_seekdir(qfuse_compat_DIR *stream, long offset)
{
    seekdir(stream->native, offset);
}
/* All native declarations have been parsed before these substitutions. */
#define dirent qfuse_compat_dirent
#define DIR qfuse_compat_DIR
#define opendir(path) qfuse_compat_opendir(path)
#define readdir(stream) qfuse_compat_readdir(stream)
#define closedir(stream) qfuse_compat_closedir(stream)
#undef dirfd
#define dirfd(stream) qfuse_compat_dirfd(stream)
#define rewinddir(stream) qfuse_compat_rewinddir(stream)
#define telldir(stream) qfuse_compat_telldir(stream)
#define seekdir(stream, offset) qfuse_compat_seekdir(stream, offset)
/* Unsupported entry points must fail at compile time, not mix the two ABIs. */
#define dircntl(...) QFUSE_DIRENT_DIRCNTL_NOT_SUPPORTED
#define _readdir_r(...) QFUSE_DIRENT_READDIR_R_NOT_SUPPORTED
#define _readdir64_r(...) QFUSE_DIRENT64_NOT_SUPPORTED
#define fdopendir(...) QFUSE_DIRENT_FDOPENDIR_NOT_SUPPORTED
#define readdir_r(...) QFUSE_DIRENT_READDIR_R_NOT_SUPPORTED
#define scandir(...) QFUSE_DIRENT_SCANDIR_NOT_SUPPORTED
#define alphasort(...) QFUSE_DIRENT_ALPHASORT_NOT_SUPPORTED
#define dirent64 QFUSE_DIRENT64_NOT_SUPPORTED
#define readdir64(...) QFUSE_DIRENT64_NOT_SUPPORTED
#define readdir64_r(...) QFUSE_DIRENT64_NOT_SUPPORTED
#endif
