/* SPDX-License-Identifier: MIT */
#ifndef QFUSE_INFLIGHT_PROTOCOL_H
#define QFUSE_INFLIGHT_PROTOCOL_H
#include <sys/types.h>
#define QF_INFLIGHT_CHUNK 4096
struct qf_inflight_event {
    pid_t caller;
    off_t offset;
};
#endif
