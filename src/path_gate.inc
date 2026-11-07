/* SPDX-License-Identifier: MIT
 * Private, portable namespace scheduler. All paths in one lease are granted
 * atomically. Conflicting queued leases retain arrival order; unrelated paths
 * can bypass them. Waiters sleep on conditions, never poll a clock.
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define QF_GATE_PATHS 3
#define QF_GATE_LIMIT 64
struct qf_path_lease {
    char *paths[QF_GATE_PATHS];
    unsigned count, base_count;
    int exclusive, granted;
    uint64_t ticket;
    int (*cancelled)(void *);
    void *cancel_data;
    pthread_cond_t ready;
    struct qf_path_lease *next;
};
struct qf_path_gate {
    pthread_mutex_t mutex;
    struct qf_path_lease *active, *head, *tail;
    unsigned active_count, waiting_count;
    uint64_t ticket;
};
static inline int qf_gate_init(struct qf_path_gate *g)
{
    memset(g, 0, sizeof(*g));
    pthread_mutexattr_t a;
    int e = pthread_mutexattr_init(&a);
    if (e)
        return e;
#ifdef __QNXNTO__
    e = pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
#endif
    if (!e)
        e = pthread_mutex_init(&g->mutex, &a);
    pthread_mutexattr_destroy(&a);
    return e;
}
/* Noncanonical spellings retain a conservative whole-namespace scope. Do not
 * normalize through a symlink, or let '..' bypass a conflicting path lease. */
static inline int qf_gate_canonical(const char *p)
{
    if (!p || p[0] != '/' || strlen(p) > 4096)
        return 0;
    if (!p[1])
        return 1;
    for (const char *s = p + 1;;) {
        const char *end = strchr(s, '/');
        size_t n = end ? (size_t)(end - s) : strlen(s);
        if (!n || (n == 1 && s[0] == '.') || (n == 2 && !memcmp(s, "..", 2)))
            return 0;
        if (!end)
            return 1;
        s = end + 1;
    }
}
static inline int qf_gate_overlap(const char *a, const char *b)
{
    if (!strcmp(a, "/") || !strcmp(b, "/"))
        return 1;
    size_t n = strlen(a), m = strlen(b), common = n < m ? n : m;
    return !memcmp(a, b, common) && (n == m || (n < m ? b[n] : a[m]) == '/');
}
static inline int qf_gate_conflicts(const struct qf_path_lease *a, const struct qf_path_lease *b)
{
    if (!a->exclusive && !b->exclusive)
        return 0;
    for (unsigned i = 0; i < a->count; i++)
        for (unsigned j = 0; j < b->count; j++)
            if (qf_gate_overlap(a->paths[i], b->paths[j]))
                return 1;
    return 0;
}
static inline void qf_gate_wake_locked(struct qf_path_gate *g)
{
    for (struct qf_path_lease *p = g->head; p; p = p->next)
        pthread_cond_signal(&p->ready);
}
static inline void qf_gate_notify(struct qf_path_gate *g)
{
    pthread_mutex_lock(&g->mutex);
    qf_gate_wake_locked(g);
    pthread_mutex_unlock(&g->mutex);
}
static inline void qf_gate_unqueue(struct qf_path_gate *g, struct qf_path_lease *l)
{
    struct qf_path_lease **link = &g->head, *previous = NULL;
    while (*link && *link != l) {
        previous = *link;
        link = &(*link)->next;
    }
    if (*link) {
        *link = l->next;
        if (g->tail == l)
            g->tail = previous;
        g->waiting_count--;
    }
    l->next = NULL;
}
static inline int qf_gate_available(struct qf_path_gate *g, struct qf_path_lease *l)
{
    for (struct qf_path_lease *p = g->active; p; p = p->next)
        if (qf_gate_conflicts(p, l))
            return 0;
    for (struct qf_path_lease *p = g->head; p && p != l; p = p->next)
        if (qf_gate_conflicts(p, l))
            return 0;
    return 1;
}
static inline void qf_gate_dispose(struct qf_path_lease *l)
{
    for (unsigned i = 0; i < l->count; i++)
        free(l->paths[i]);
    pthread_cond_destroy(&l->ready);
    memset(l, 0, sizeof(*l));
}
static inline int qf_gate_enter(struct qf_path_gate *g, struct qf_path_lease *l,
                                const char *const *paths, unsigned count, int exclusive,
                                int (*cancelled)(void *), void *data, int wait)
{
    if (!count || count > QF_GATE_PATHS)
        return EINVAL;
    memset(l, 0, sizeof(*l));
    int e = pthread_cond_init(&l->ready, NULL);
    if (e)
        return e;
    l->exclusive = exclusive;
    l->cancelled = cancelled;
    l->cancel_data = data;
    for (unsigned i = 0; i < count; i++) {
        if (!paths[i] || paths[i][0] != '/' || strlen(paths[i]) > 4096) {
            qf_gate_dispose(l);
            return EINVAL;
        }
        l->paths[i] = strdup(qf_gate_canonical(paths[i]) ? paths[i] : "/");
        if (!l->paths[i]) {
            qf_gate_dispose(l);
            return ENOMEM;
        }
        l->count++;
    }
    l->base_count = count;
    pthread_mutex_lock(&g->mutex);
    if (g->active_count + g->waiting_count == QF_GATE_LIMIT) {
        e = EAGAIN;
        goto fail;
    }
    l->ticket = ++g->ticket;
    if (!wait && !qf_gate_available(g, l)) {
        e = EBUSY;
        goto fail;
    }
    if (g->tail)
        g->tail->next = l;
    else
        g->head = l;
    g->tail = l;
    g->waiting_count++;
    for (;;) {
        if (cancelled && cancelled(data)) {
            e = ECANCELED;
            break;
        }
        if (qf_gate_available(g, l)) {
            qf_gate_unqueue(g, l);
            l->next = g->active;
            g->active = l;
            g->active_count++;
            l->granted = 1;
            pthread_mutex_unlock(&g->mutex);
            return 0;
        }
        e = pthread_cond_wait(&l->ready, &g->mutex);
        if (e)
            break;
    }
    qf_gate_unqueue(g, l);
    qf_gate_wake_locked(g);
fail:
    pthread_mutex_unlock(&g->mutex);
    qf_gate_dispose(l);
    return e;
}
static inline void qf_gate_leave(struct qf_path_gate *g, struct qf_path_lease *l)
{
    if (!l->granted)
        return;
    pthread_mutex_lock(&g->mutex);
    struct qf_path_lease **p = &g->active;
    while (*p && *p != l)
        p = &(*p)->next;
    if (!*p)
        abort();
    *p = l->next;
    g->active_count--;
    l->granted = 0;
    qf_gate_wake_locked(g);
    pthread_mutex_unlock(&g->mutex);
    qf_gate_dispose(l);
}
/* Hidden-name reservation must not wait while owning another write lease.
 * Later waiters cannot prevent completion of an already granted transaction. */
static inline int qf_gate_extend(struct qf_path_gate *g, struct qf_path_lease *l, const char *path)
{
    if (!l->granted || !l->exclusive || l->count == QF_GATE_PATHS || !qf_gate_canonical(path))
        return EINVAL;
    char *copy = strdup(path);
    if (!copy)
        return ENOMEM;
    struct qf_path_lease extra = {.paths = {copy}, .count = 1, .exclusive = 1};
    int e = 0;
    pthread_mutex_lock(&g->mutex);
    for (struct qf_path_lease *p = g->active; p; p = p->next)
        if (p != l && qf_gate_conflicts(p, &extra))
            e = EBUSY;
    for (struct qf_path_lease *p = g->head; p; p = p->next)
        if (p->ticket < l->ticket && qf_gate_conflicts(p, &extra))
            e = EBUSY;
    if (!e)
        l->paths[l->count++] = copy;
    pthread_mutex_unlock(&g->mutex);
    if (e)
        free(copy);
    return e;
}
static inline void qf_gate_unextend(struct qf_path_gate *g, struct qf_path_lease *l)
{
    pthread_mutex_lock(&g->mutex);
    while (l->count > l->base_count)
        free(l->paths[--l->count]);
    qf_gate_wake_locked(g);
    pthread_mutex_unlock(&g->mutex);
}
static inline void qf_gate_counts(struct qf_path_gate *g, unsigned *active, unsigned *waiting)
{
    pthread_mutex_lock(&g->mutex);
    *active = g->active_count;
    *waiting = g->waiting_count;
    pthread_mutex_unlock(&g->mutex);
}
static inline int qf_gate_destroy(struct qf_path_gate *g)
{
    if (g->active_count || g->waiting_count)
        return EBUSY;
    return pthread_mutex_destroy(&g->mutex);
}
