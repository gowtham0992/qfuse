/* SPDX-License-Identifier: MIT
 * Local root-only management protocol, independent of the mounted data path.
 */
#ifndef QFUSE_ADMIN_H
#define QFUSE_ADMIN_H
#include <stdint.h>
#define QFUSE_ADMIN_MAGIC UINT32_C(0x51464131)
#define QFUSE_ADMIN_VERSION 1
#define QFUSE_ADMIN_GROUP 1
#define QFUSE_ADMIN_FORCE 2
#define QFUSE_ADMIN_STATUS 1
#define QFUSE_ADMIN_STOP 2
#define QFUSE_ADMIN_KILL 3
#define QFUSE_ADMIN_STAGING 0
#define QFUSE_ADMIN_SERVING 1
#define QFUSE_ADMIN_STOPPING 2
#define QFUSE_ADMIN_FAILED 3
struct qfuse_admin_advert {
    uint32_t magic, version, pid, channel;
    unsigned char instance[16];
};
struct qfuse_admin_request {
    uint32_t magic, version, command, flags;
    unsigned char instance[16];
};
struct qfuse_admin_status {
    uint32_t magic, version, size, phase;
    uint32_t pid, channel, group_leader, active_operations, operation_limit;
    uint32_t physical_ocbs, ocb_limit, cancelled_waits, cleanup_pending;
    unsigned char instance[16];
    char mountpoint[4097];
};
#endif
