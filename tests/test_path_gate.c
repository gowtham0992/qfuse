/* SPDX-License-Identifier: MIT — independent scheduling and lifetime checks. */
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <sched.h>
#include "../src/path_gate.inc"
static struct qf_path_gate gate;
static int cancelled(void *p)
{
    return atomic_load((_Atomic int *)p);
}
struct worker {
    const char *path;
    int exclusive, result;
    _Atomic int cancel, acquired, release;
};
static void *run(void *p)
{
    struct worker *w = p;
    struct qf_path_lease l;
    w->result = qf_gate_enter(&gate, &l, &w->path, 1, w->exclusive, cancelled, &w->cancel, 1);
    if (!w->result) {
        atomic_store(&w->acquired, 1);
        while (!atomic_load(&w->release))
            sched_yield();
        qf_gate_leave(&gate, &l);
    }
    return NULL;
}
static void waiting(unsigned expected)
{
    struct timespec begin, now;
    clock_gettime(CLOCK_MONOTONIC, &begin);
    for (;;) {
        unsigned a, n;
        qf_gate_counts(&gate, &a, &n);
        if (n == expected)
            return;
        clock_gettime(CLOCK_MONOTONIC, &now);
        assert(now.tv_sec - begin.tv_sec < 5);
        sched_yield();
    }
}
static void acquired(struct worker *w)
{
    struct timespec begin, now;
    clock_gettime(CLOCK_MONOTONIC, &begin);
    while (!atomic_load(&w->acquired)) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        assert(now.tv_sec - begin.tv_sec < 5);
        sched_yield();
    }
}
int main(void)
{
    assert(!qf_gate_init(&gate));
    struct qf_path_lease a, b;
    const char *one[] = {"/dir/file"}, *sibling[] = {"/dir/new"};
    assert(!qf_gate_enter(&gate, &a, one, 1, 0, NULL, NULL, 0));
    assert(!qf_gate_enter(&gate, &b, sibling, 1, 1, NULL, NULL, 0));
    qf_gate_leave(&gate, &b);
    const char *parent[] = {"/dir"}, *similar[] = {"/directory"};
    assert(qf_gate_enter(&gate, &b, parent, 1, 1, NULL, NULL, 0) == EBUSY);
    assert(!qf_gate_enter(&gate, &b, similar, 1, 1, NULL, NULL, 0));
    qf_gate_leave(&gate, &b);
    const char *rename[] = {"/else/file", "/dir/file"};
    assert(qf_gate_enter(&gate, &b, rename, 2, 1, NULL, NULL, 0) == EBUSY);
    assert(!qf_gate_enter(&gate, &b, rename, 1, 1, NULL, NULL, 0));
    qf_gate_leave(&gate, &b);
    struct worker writer = {.path = "/dir", .exclusive = 1};
    struct worker reader = {.path = "/dir/late"};
    pthread_t t1, t2;
    assert(!pthread_create(&t1, NULL, run, &writer));
    waiting(1);
    assert(!pthread_create(&t2, NULL, run, &reader));
    waiting(2);
    assert(!atomic_load(&reader.acquired));
    assert(!qf_gate_enter(&gate, &b, similar, 1, 1, NULL, NULL, 0));
    qf_gate_leave(&gate, &b);
    qf_gate_leave(&gate, &a);
    acquired(&writer);
    assert(!atomic_load(&reader.acquired));
    atomic_store(&writer.release, 1);
    assert(!pthread_join(t1, NULL));
    acquired(&reader);
    atomic_store(&reader.release, 1);
    assert(!pthread_join(t2, NULL));
    puts("PASS path gate: independent siblings, atomic rename and conflicting FIFO fairness");

    assert(!qf_gate_enter(&gate, &a, one, 1, 0, NULL, NULL, 0));
    memset(&writer, 0, sizeof(writer));
    writer.path = "/dir";
    writer.exclusive = 1;
    memset(&reader, 0, sizeof(reader));
    reader.path = "/dir/late";
    assert(!pthread_create(&t1, NULL, run, &writer));
    waiting(1);
    assert(!pthread_create(&t2, NULL, run, &reader));
    waiting(2);
    atomic_store(&writer.cancel, 1);
    qf_gate_notify(&gate);
    assert(!pthread_join(t1, NULL) && writer.result == ECANCELED);
    acquired(&reader);
    atomic_store(&reader.release, 1);
    assert(!pthread_join(t2, NULL));
    qf_gate_leave(&gate, &a);
    puts("PASS path gate: cancellation removes a queued writer and wakes compatible readers");

    assert(!qf_gate_enter(&gate, &a, one, 1, 1, NULL, NULL, 0));
    memset(&reader, 0, sizeof(reader));
    reader.path = "/dir";
    assert(!pthread_create(&t2, NULL, run, &reader));
    waiting(1);
    assert(!qf_gate_extend(&gate, &a, "/dir/.hidden"));
    qf_gate_unextend(&gate, &a);
    qf_gate_leave(&gate, &a);
    acquired(&reader);
    atomic_store(&reader.release, 1);
    assert(!pthread_join(t2, NULL));
    const char *alias[] = {"/dir/../else"};
    assert(!qf_gate_enter(&gate, &a, alias, 1, 0, NULL, NULL, 0));
    assert(qf_gate_enter(&gate, &b, similar, 1, 1, NULL, NULL, 0) == EBUSY);
    qf_gate_leave(&gate, &a);
    struct qf_path_lease many[QF_GATE_LIMIT];
    for (unsigned i = 0; i < QF_GATE_LIMIT; i++)
        assert(!qf_gate_enter(&gate, &many[i], one, 1, 0, NULL, NULL, 0));
    assert(qf_gate_enter(&gate, &b, one, 1, 0, NULL, NULL, 0) == EAGAIN);
    for (unsigned i = 0; i < QF_GATE_LIMIT; i++)
        qf_gate_leave(&gate, &many[i]);
    assert(!qf_gate_destroy(&gate));
    puts("PASS path gate: hidden-name extension, alias fallback, resource bounds and empty "
         "teardown");
    return 0;
}
