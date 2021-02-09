#include "fdm.h"

#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>

#include <tllist.h>

#define LOG_MODULE "fdm"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "debug.h"
#include "xmalloc.h"

struct handler {
    enum {
        HANDLER_ACTIVE,
        HANDLER_DEFERRED_DELETE,
        HANDLER_DEFERRED_DELETE_AND_CLOSE
    } status;
    fdm_handler_t callback;
    void *callback_data;
};

struct hook {
    fdm_hook_t callback;
    void *callback_data;
};

typedef tll(struct hook) hooks_t;

struct fdm {
    /*
     * Paired arrays of poll() data and the associated callbacks.
     *
     * Both arrays have <size> number of elements. The first <count>
     * elements are valid/in use.
     *
     * <max_count> is for debugging/statistics, and tracks the maximum
     * number of simultaniously active FDs.
     */
    struct pollfd *fds;
    struct handler *handlers;
    size_t count;
    size_t size;
    size_t max_count;

    hooks_t hooks_low;
    hooks_t hooks_normal;
    hooks_t hooks_high;
    bool is_polling;
};

static const size_t min_slot_count = 32;

struct fdm *
fdm_init(void)
{
    struct fdm *fdm = malloc(sizeof(*fdm));
    if (unlikely(fdm == NULL)) {
        LOG_ERRNO("malloc() failed");
        return NULL;
    }

    struct pollfd *fds = malloc(min_slot_count * sizeof(fds[0]));
    struct handler *handlers = malloc(min_slot_count * sizeof(handlers[0]));

    if (fds == NULL || handlers == NULL) {
        LOG_ERRNO("failed to allocate initial array of FD handlers");
        free(handlers);
        free(fds);
        free(fdm);
        return NULL;
    }

    *fdm = (struct fdm){
        .is_polling = false,
        .fds = fds,
        .handlers = handlers,
        .size = min_slot_count,
        .hooks_low = tll_init(),
        .hooks_normal = tll_init(),
        .hooks_high = tll_init(),
    };
    return fdm;
}

void
fdm_destroy(struct fdm *fdm)
{
    if (fdm == NULL)
        return;

    LOG_DBG("max FDs registered: %zu", fdm->max_count);

    if (fdm->count > 0)
        LOG_WARN("FD list not empty");

    if (tll_length(fdm->hooks_low) > 0 ||
        tll_length(fdm->hooks_normal) > 0 ||
        tll_length(fdm->hooks_high) > 0)
    {
        LOG_WARN("hook list not empty");
    }

    xassert(fdm->count == 0);
    xassert(tll_length(fdm->hooks_low) == 0);
    xassert(tll_length(fdm->hooks_normal) == 0);
    xassert(tll_length(fdm->hooks_high) == 0);

    free(fdm->fds);
    free(fdm->handlers);
    tll_free(fdm->hooks_low);
    tll_free(fdm->hooks_normal);
    tll_free(fdm->hooks_high);
    free(fdm);
}

bool
fdm_add(struct fdm *fdm, int fd, int events, fdm_handler_t cb, void *data)
{
#if defined(_DEBUG)
    for (size_t i = 0; i < fdm->count; i++) {
        if (fdm->fds[i].fd == fd) {
            LOG_ERR("FD=%d already registered", fd);
            xassert(false);
            return false;
        }
    }
#endif

    if (fdm->count >= fdm->size) {
        /* No free slot - increase number of pollfds + handlers */

        size_t old_size = fdm->size;
        size_t new_size = old_size * 2;

        fdm->fds = xrealloc(fdm->fds, new_size * sizeof(fdm->fds[0]));
        fdm->handlers = xrealloc(fdm->handlers, new_size * sizeof(fdm->handlers[0]));
        fdm->size = new_size;
    }

    xassert(fdm->count < fdm->size);

    struct pollfd *pfd = &fdm->fds[fdm->count];
    struct handler *handler = &fdm->handlers[fdm->count];
    
    pfd->fd = fd;
    pfd->events = events;

    handler->callback = cb;
    handler->callback_data = data;
    handler->status = HANDLER_ACTIVE;

    if (++fdm->count > fdm->max_count)
        fdm->max_count = fdm->count;

    return true;
}

static void
deferred_delete(struct fdm *fdm, size_t idx)
{
    xassert(!fdm->is_polling);
    xassert(idx < fdm->count);

    struct handler *handler = &fdm->handlers[idx];
    xassert(fdm->handlers[idx].status != HANDLER_ACTIVE);

    if (handler->status == HANDLER_DEFERRED_DELETE_AND_CLOSE) {
        xassert(fdm->fds[idx].fd >= 0);
        close(fdm->fds[idx].fd);
    }

    const size_t remaining = fdm->count - (idx + 1);

    memmove(&fdm->fds[idx], &fdm->fds[idx + 1],
            remaining * sizeof(fdm->fds[0]));

    memmove(&fdm->handlers[idx], &fdm->handlers[idx + 1],
            remaining * sizeof(fdm->handlers[0]));

    fdm->count--;
}

static bool
fdm_del_internal(struct fdm *fdm, int fd, bool close_fd)
{
    if (fd == -1)
        return true;

    for (size_t i = 0; i < fdm->count; i++) {
        struct pollfd *pfd = &fdm->fds[i];
        struct handler *handler = &fdm->handlers[i];

        if (pfd->fd != fd)
            continue;

        handler->status = close_fd
            ? HANDLER_DEFERRED_DELETE_AND_CLOSE
            : HANDLER_DEFERRED_DELETE;

        if (!fdm->is_polling)
            deferred_delete(fdm, i);

        return true;
    }

    LOG_ERR("no such FD: %d", fd);
    if (close_fd)
        close(fd);
    return false;
}

bool
fdm_del(struct fdm *fdm, int fd)
{
    return fdm_del_internal(fdm, fd, true);
}

bool
fdm_del_no_close(struct fdm *fdm, int fd)
{
    return fdm_del_internal(fdm, fd, false);
}

bool
fdm_event_add(struct fdm *fdm, int fd, int events)
{
    for (size_t i = 0; i < fdm->count; i++) {
        struct pollfd *pfd = &fdm->fds[i];

        if (pfd->fd != fd)
            continue;

        pfd->events |= events;
        return true;
    }

    LOG_ERR("FD=%d not registered with the FDM", fd);
    return false;
}

bool
fdm_event_del(struct fdm *fdm, int fd, int events)
{
    for (size_t i = 0; i < fdm->count; i++) {
        struct pollfd *pfd = &fdm->fds[i];

        if (pfd->fd != fd)
            continue;

        pfd->events &= ~events;
        return true;
    }

    LOG_ERR("FD=%d not registered with the FDM", fd);
    return false;
}

static hooks_t *
hook_priority_to_list(struct fdm *fdm, enum fdm_hook_priority priority)
{
    switch (priority) {
    case FDM_HOOK_PRIORITY_LOW:    return &fdm->hooks_low;
    case FDM_HOOK_PRIORITY_NORMAL: return &fdm->hooks_normal;
    case FDM_HOOK_PRIORITY_HIGH:   return &fdm->hooks_high;
    }

    xassert(false);
    return NULL;
}

bool
fdm_hook_add(struct fdm *fdm, fdm_hook_t hook, void *data,
             enum fdm_hook_priority priority)
{
    hooks_t *hooks = hook_priority_to_list(fdm, priority);

#if defined(_DEBUG)
    tll_foreach(*hooks, it) {
        if (it->item.callback == hook) {
            LOG_ERR("hook=0x%" PRIxPTR " already registered", (uintptr_t)hook);
            return false;
        }
    }
#endif

    tll_push_back(*hooks, ((struct hook){hook, data}));
    return true;
}

bool
fdm_hook_del(struct fdm *fdm, fdm_hook_t hook, enum fdm_hook_priority priority)
{
    hooks_t *hooks = hook_priority_to_list(fdm, priority);

    tll_foreach(*hooks, it) {
        if (it->item.callback != hook)
            continue;

        tll_remove(*hooks, it);
        return true;
    }

    LOG_WARN("hook=0x%" PRIxPTR " not registered", (uintptr_t)hook);
    return false;
}

bool
fdm_poll(struct fdm *fdm)
{
    xassert(!fdm->is_polling && "nested calls to fdm_poll() not allowed");
    if (fdm->is_polling) {
        LOG_ERR("nested calls to fdm_poll() not allowed");
        return false;
    }

    tll_foreach(fdm->hooks_high, it) {
        LOG_DBG(
            "executing high priority hook 0x%" PRIxPTR" (fdm=%p, data=%p)",
            (uintptr_t)it->item.callback, (void *)fdm,
            (void *)it->item.callback_data);
        it->item.callback(fdm, it->item.callback_data);
    }
    tll_foreach(fdm->hooks_normal, it) {
        LOG_DBG(
            "executing normal priority hook 0x%" PRIxPTR " (fdm=%p, data=%p)",
            (uintptr_t)it->item.callback, (void *)fdm,
            (void *)it->item.callback_data);
        it->item.callback(fdm, it->item.callback_data);
    }
    tll_foreach(fdm->hooks_low, it) {
        LOG_DBG(
            "executing low priority hook 0x%" PRIxPTR " (fdm=%p, data=%p)",
            (uintptr_t)it->item.callback, (void *)fdm,
            (void *)it->item.callback_data);
        it->item.callback(fdm, it->item.callback_data);
    }

    int r = poll(fdm->fds, fdm->count, -1);
    if (unlikely(r < 0)) {
        if (errno == EINTR)
            return true;

        LOG_ERRNO("failed to poll");
        return false;
    }

    bool ret = true;

    fdm->is_polling = true;
    for (int i = 0, matched = 0; matched < r; i++) {
        xassert(i < fdm->count);

        struct pollfd *pfd = &fdm->fds[i];
        if (pfd->revents == 0)
            continue;

        matched++;

        struct handler *handler = &fdm->handlers[i];
        if (handler->status == HANDLER_ACTIVE) {
            if (!handler->callback(
                    fdm, pfd->fd, pfd->revents, handler->callback_data))
            {
                ret = false;
                break;
            }
        }
    }

    fdm->is_polling = false;

    size_t count = fdm->count;
    size_t i = 0;

    while (i < count) {
        struct handler *handler = &fdm->handlers[i];
        if (handler->status == HANDLER_ACTIVE) {
            i++;
            continue;
        }

        deferred_delete(fdm, i);
        count--;
    }
    
    return ret;
}
