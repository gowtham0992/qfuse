/* SPDX-License-Identifier: MIT */
#ifndef QFUSE_CONTROL_H
#define QFUSE_CONTROL_H
#include <stdint.h>
#include <devctl.h>
#define QFUSE_CONTROL_MAGIC UINT32_C(0x51465553)
struct qfuse_mount_info {
    uint32_t magic;
    uint32_t version;
    char mountpoint[4097];
};
#define QFUSE_CONTROL_GET_INFO __DIOF(_DCMD_MISC, 225, struct qfuse_mount_info)
#endif
