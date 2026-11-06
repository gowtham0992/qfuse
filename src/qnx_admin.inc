/* SPDX-License-Identifier: MIT
 * A separate native channel stays available when data callbacks are stalled.
 * No management operation invokes a filesystem callback.
 */
static int admin_channel = -1;
static pthread_t admin_thread;
static int admin_started, admin_wake = -1;
static _Atomic int admin_stopping;
static unsigned char admin_instance[16];
static int admin_authorized(const struct _msg_info *info)
{
    if (info->nd != ND_LOCAL_NODE)
        return 0;
    struct _client_info *client = NULL;
    if (ConnectClientInfoExt(info->scoid, &client, 0) == -1)
        return 0;
    int allowed = client->cred.euid == 0 && client->pid == info->pid;
    ClientInfoExtFree(&client);
    return allowed;
}
static void admin_snapshot(struct qfuse_admin_status *s)
{
    memset(s, 0, sizeof(*s));
    s->magic = QFUSE_ADMIN_MAGIC;
    s->version = QFUSE_ADMIN_VERSION;
    s->size = sizeof(*s);
    s->phase = atomic_load(&admin_phase);
    s->pid = getpid();
    s->channel = admin_channel;
    s->group_leader = getpgrp() == getpid();
    s->operation_limit = REQUEST_LIMIT;
    s->ocb_limit = OCB_LIMIT;
    memcpy(s->instance, admin_instance, sizeof(s->instance));
    state_lock();
    s->physical_ocbs = server.physical_ocbs;
    if (server.owner && server.owner->mountpoint)
        strcpy(s->mountpoint, server.owner->mountpoint);
    state_unlock();
    pthread_mutex_lock(&pending_mutex);
    s->active_operations = payload_requests;
    s->cancelled_waits = cancelled_requests;
    pthread_mutex_unlock(&pending_mutex);
    pthread_mutex_lock(&release_mutex);
    s->cleanup_pending = release_count + release_active;
    pthread_mutex_unlock(&release_mutex);
}
static void *admin_loop(void *unused)
{
    (void)unused;
    union {
        struct _pulse pulse;
        io_open_t connect;
        struct qfuse_admin_request request;
        uint16_t type;
    } message;
    while (!atomic_load(&admin_stopping)) {
        struct _msg_info info;
        rcvid_t id = MsgReceive(admin_channel, &message, sizeof(message), &info);
        if (id == -1) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (atomic_load(&admin_stopping)) {
            if (id > 0)
                MsgError(id, EIO);
            break;
        }
        if (!id) {
            if (message.pulse.code == _PULSE_CODE_DISCONNECT)
                ConnectDetach(message.pulse.scoid);
            continue;
        }
        if (info.srcmsglen != sizeof(struct qfuse_admin_request) ||
            message.request.magic != QFUSE_ADMIN_MAGIC ||
            message.request.version != QFUSE_ADMIN_VERSION) {
            MsgError(id, EINVAL);
            continue;
        }
        if (!admin_authorized(&info)) {
            MsgError(id, EPERM);
            continue;
        }
        unsigned command = message.request.command;
        if (command < QFUSE_ADMIN_STATUS || command > QFUSE_ADMIN_KILL ||
            (message.request.flags & ~(QFUSE_ADMIN_GROUP | QFUSE_ADMIN_FORCE)) ||
            (command == QFUSE_ADMIN_STATUS && message.request.flags) ||
            (command == QFUSE_ADMIN_STOP && (message.request.flags & ~QFUSE_ADMIN_FORCE)) ||
            (command == QFUSE_ADMIN_KILL && (message.request.flags & ~QFUSE_ADMIN_GROUP))) {
            MsgError(id, EINVAL);
            continue;
        }
        if (command != QFUSE_ADMIN_STATUS &&
            memcmp(message.request.instance, admin_instance, sizeof(admin_instance))) {
            MsgError(id, ESTALE);
            continue;
        }
        if (command == QFUSE_ADMIN_STOP && !(message.request.flags & QFUSE_ADMIN_FORCE)) {
            int busy = 0;
            state_lock();
            for (struct node *n = server.nodes; n; n = n->next)
                if (n->opens) {
                    busy = 1;
                    break;
                }
            state_unlock();
            if (busy) {
                MsgError(id, EBUSY);
                continue;
            }
        }
        struct qfuse_admin_status status;
        admin_snapshot(&status);
        if (command == QFUSE_ADMIN_STOP) {
            atomic_store(&admin_phase, QFUSE_ADMIN_STOPPING);
            status.phase = QFUSE_ADMIN_STOPPING;
        }
        if (MsgReply(id, EOK, &status, sizeof(status)) == -1)
            continue;
        if (command == QFUSE_ADMIN_STOP)
            fuse_exit(server.owner);
        else if (command == QFUSE_ADMIN_KILL) {
            if ((message.request.flags & QFUSE_ADMIN_GROUP) && getpgrp() == getpid())
                kill(-getpgrp(), SIGKILL);
            else
                kill(getpid(), SIGKILL);
        }
    }
    return NULL;
}
static int admin_start(void)
{
    int random = open("/dev/urandom", O_RDONLY);
    if (random == -1)
        return -errno;
    size_t position = 0;
    while (position < sizeof(admin_instance)) {
        ssize_t n = read(random, admin_instance + position, sizeof(admin_instance) - position);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            int error = errno ? errno : EIO;
            close(random);
            return -error;
        }
        position += n;
    }
    close(random);
    /* No UNBLOCK flag: these fixed-size RPCs retain no client buffers and
     * kernel timeouts must also work if the whole process is stopped. Every
     * client uses a fresh connection; disconnect SCIDs are reaped by this loop. */
    admin_channel = ChannelCreate(_NTO_CHF_DISCONNECT);
    if (admin_channel == -1)
        return -errno;
    admin_wake = ConnectAttach(ND_LOCAL_NODE, 0, admin_channel, _NTO_SIDE_CHANNEL, 0);
    if (admin_wake == -1) {
        int e = errno;
        ChannelDestroy(admin_channel);
        admin_channel = -1;
        return -e;
    }
    atomic_store(&admin_stopping, 0);
    atomic_store(&admin_phase, QFUSE_ADMIN_STAGING);
    sigset_t blocked, saved;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGHUP);
    int e = pthread_sigmask(SIG_BLOCK, &blocked, &saved);
    if (!e) {
        e = pthread_create(&admin_thread, NULL, admin_loop, NULL);
        pthread_sigmask(SIG_SETMASK, &saved, NULL);
    }
    if (e) {
        ConnectDetach(admin_wake);
        admin_wake = -1;
        ChannelDestroy(admin_channel);
        admin_channel = -1;
        return -e;
    }
    admin_started = 1;
    const char *advertised = getenv("QFUSE_ADMIN_FD");
    if (advertised) {
        char *end;
        long fd = strtol(advertised, &end, 10);
        struct stat st;
        if (*advertised && !*end && fd >= 3 && fd <= INT_MAX && !fstat((int)fd, &st) &&
            S_ISFIFO(st.st_mode)) {
            struct qfuse_admin_advert advert;
            memset(&advert, 0, sizeof(advert));
            advert.magic = QFUSE_ADMIN_MAGIC;
            advert.version = QFUSE_ADMIN_VERSION;
            advert.pid = getpid();
            advert.channel = admin_channel;
            memcpy(advert.instance, admin_instance, sizeof(advert.instance));
            (void)write((int)fd, &advert, sizeof(advert));
            close((int)fd);
        }
        unsetenv("QFUSE_ADMIN_FD");
    }
    return 0;
}
static void admin_stop(void)
{
    if (admin_channel == -1)
        return;
    if (admin_started) {
        atomic_store(&admin_stopping, 1);
        MsgSendPulse(admin_wake, -1, _PULSE_CODE_MINAVAIL, 0);
        pthread_join(admin_thread, NULL);
        admin_started = 0;
    }
    if (admin_wake != -1) {
        ConnectDetach(admin_wake);
        admin_wake = -1;
    }
    ChannelDestroy(admin_channel);
    admin_channel = -1;
}
