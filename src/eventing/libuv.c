/*
 * Authored by Alex Hultman, 2018-2019.
 * Intellectual property of third-party.

 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at

 *     http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "libusockets.h"
#include "internal/common.h"
#include <stdlib.h>

#ifdef LIBUS_USE_LIBUV

// poll dispatch
static void poll_cb(uv_poll_t *p, int status, int events) {
    us_internal_dispatch_ready_poll((struct us_poll *) p, status < 0, events);
}

static void prepare_cb(uv_prepare_t *p) {
    struct us_loop *loop = p->data;
    us_internal_loop_pre(loop);
}

/* Note: libuv timers execute AFTER the post callback */
static void check_cb(uv_check_t *p) {
    struct us_loop *loop = p->data;
    us_internal_loop_post(loop);
}

static void close_cb_free(uv_handle_t *h) {
    //printf("close_cb_free\n");
    free(h->data);
}

static void timer_cb(uv_timer_t *t) {
    struct us_internal_callback *cb = t->data;
    cb->cb(cb);
}

static void async_cb(uv_async_t *a) {
    struct us_internal_callback *cb = a->data;
    // internal asyncs give their loop, not themselves
    cb->cb((struct us_internal_callback *) cb->loop);
}

// poll
void us_poll_init(struct us_poll *p, LIBUS_SOCKET_DESCRIPTOR fd, int poll_type) {
    p->poll_type = poll_type;
    p->fd = fd;
}

void us_poll_free(struct us_poll *p, struct us_loop *loop) {
    if (uv_is_closing((uv_handle_t *) &p->uv_p)) {
        p->uv_p.data = p;
    } else {
        free(p);
    }
}

void us_poll_start(struct us_poll *p, struct us_loop *loop, int events) {
    p->poll_type = us_internal_poll_type(p) | ((events & LIBUS_SOCKET_READABLE) ? POLL_TYPE_POLLING_IN : 0) | ((events & LIBUS_SOCKET_WRITABLE) ? POLL_TYPE_POLLING_OUT : 0);

    uv_poll_init_socket(loop->uv_loop, &p->uv_p, p->fd);
    uv_poll_start(&p->uv_p, events, poll_cb);
}

void us_poll_change(struct us_poll *p, struct us_loop *loop, int events) {
    if (us_poll_events(p) != events) {
        p->poll_type = us_internal_poll_type(p) | ((events & LIBUS_SOCKET_READABLE) ? POLL_TYPE_POLLING_IN : 0) | ((events & LIBUS_SOCKET_WRITABLE) ? POLL_TYPE_POLLING_OUT : 0);

        uv_poll_start(&p->uv_p, events, poll_cb);
    }
}

void us_poll_stop(struct us_poll *p, struct us_loop *loop) {
    uv_poll_stop(&p->uv_p);

    // close but not free is needed here
    p->uv_p.data = 0;
    uv_close((uv_handle_t *) &p->uv_p, close_cb_free); // needed here
}

int us_poll_events(struct us_poll *p) {
    return ((p->poll_type & POLL_TYPE_POLLING_IN) ? LIBUS_SOCKET_READABLE : 0) | ((p->poll_type & POLL_TYPE_POLLING_OUT) ? LIBUS_SOCKET_WRITABLE : 0);
}

unsigned int us_internal_accept_poll_event(struct us_poll *p) {
    return 0;
}

int us_internal_poll_type(struct us_poll *p) {
    return p->poll_type & 3;
}

void us_internal_poll_set_type(struct us_poll *p, int poll_type) {
    p->poll_type = poll_type | (p->poll_type & 12);
}

LIBUS_SOCKET_DESCRIPTOR us_poll_fd(struct us_poll *p) {
    /*uv_os_fd_t fd = LIBUS_SOCKET_ERROR;
    uv_fileno((uv_handle_t *) &p->uv_p, &fd);
    return (LIBUS_SOCKET_DESCRIPTOR) fd;*/

    return p->fd;
}

struct us_loop *us_create_loop(int default_hint, void (*wakeup_cb)(struct us_loop *loop), void (*pre_cb)(struct us_loop *loop), void (*post_cb)(struct us_loop *loop), unsigned int ext_size) {
    struct us_loop *loop = (struct us_loop *) malloc(sizeof(struct us_loop) + ext_size);

    loop->uv_loop = default_hint ? uv_default_loop() : uv_loop_new();
    loop->is_default = default_hint;

    loop->uv_pre = malloc(sizeof(uv_prepare_t));
    uv_prepare_init(loop->uv_loop, loop->uv_pre);
    uv_prepare_start(loop->uv_pre, prepare_cb);
    uv_unref((uv_handle_t *) loop->uv_pre);
    loop->uv_pre->data = loop;

    loop->uv_check = malloc(sizeof(uv_check_t));
    uv_check_init(loop->uv_loop, loop->uv_check);
    uv_unref((uv_handle_t *) loop->uv_check);
    uv_check_start(loop->uv_check, check_cb);
    loop->uv_check->data = loop;

    // here we create two unreffed handles - timer and async
    us_internal_loop_data_init(loop, wakeup_cb, pre_cb, post_cb);

    // if we do not own this loop, we need to integrate and set up timer
    if (default_hint) {
        us_loop_integrate(loop);
    }

    return loop;
}

// based on if this was default loop or not
void us_loop_free(struct us_loop *loop) {
    //printf("us_loop_free\n");

    // ref and close down prepare and check
    uv_ref((uv_handle_t *) loop->uv_pre);
    uv_prepare_stop(loop->uv_pre);
    loop->uv_pre->data = loop->uv_pre;
    uv_close((uv_handle_t *) loop->uv_pre, close_cb_free);

    uv_ref((uv_handle_t *) loop->uv_check);
    uv_check_stop(loop->uv_check);
    loop->uv_check->data = loop->uv_check;
    uv_close((uv_handle_t *) loop->uv_check, close_cb_free);

    us_internal_loop_data_free(loop);

    // we need to run the loop one last round to call all close callbacks
    // we cannot do this if we do not own the loop, default
    if (!loop->is_default) {
        uv_run(loop->uv_loop, UV_RUN_NOWAIT);
        uv_loop_delete(loop->uv_loop);
    }

    // now we can free our part
    free(loop);
}

void us_loop_run(struct us_loop *loop) {
    us_loop_integrate(loop);

    uv_run(loop->uv_loop, UV_RUN_DEFAULT);
}

struct us_poll *us_create_poll(struct us_loop *loop, int fallthrough, unsigned int ext_size) {
    return malloc(sizeof(struct us_poll) + ext_size);
}

// this one is broken, see us_poll
struct us_poll *us_poll_resize(struct us_poll *p, struct us_loop *loop, unsigned int ext_size) {

    // do not support it yet
    return p;

    struct us_poll *new_p = realloc(p, sizeof(struct us_poll) + ext_size);
    if (p != new_p) {
        new_p->uv_p.data = new_p;
    }

    return new_p;
}

// timer
struct us_timer *us_create_timer(struct us_loop *loop, int fallthrough, unsigned int ext_size) {
    struct us_internal_callback *cb = malloc(sizeof(struct us_internal_callback) + sizeof(uv_timer_t) + ext_size);

    cb->loop = loop;
    cb->cb_expects_the_loop = 0;

    uv_timer_t *uv_timer = (uv_timer_t *) (cb + 1);
    uv_timer_init(loop->uv_loop, uv_timer);
    uv_timer->data = cb;

    if (fallthrough) {
        uv_unref((uv_handle_t *) uv_timer);
    }

    return (struct us_timer *) cb;
}

void *us_timer_ext(struct us_timer *timer) {
    return ((struct us_internal_callback *) timer) + 1;
}

void us_timer_close(struct us_timer *t) {
    struct us_internal_callback *cb = (struct us_internal_callback *) t;

    uv_timer_t *uv_timer = (uv_timer_t *) (cb + 1);

    // always ref the timer before closing it
    uv_ref((uv_handle_t *) uv_timer);

    uv_timer_stop(uv_timer);

    uv_timer->data = cb;
    uv_close((uv_handle_t *) uv_timer, close_cb_free);
}

void us_timer_set(struct us_timer *t, void (*cb)(struct us_timer *t), int ms, int repeat_ms) {
    struct us_internal_callback *internal_cb = (struct us_internal_callback *) t;

    internal_cb->cb = (void(*)(struct us_internal_callback *)) cb;

    uv_timer_t *uv_timer = (uv_timer_t *) (internal_cb + 1);
    if (!ms) {
        uv_timer_stop(uv_timer);
    } else {
        uv_timer_start(uv_timer, timer_cb, ms, repeat_ms);
    }
}

struct us_loop *us_timer_loop(struct us_timer *t) {
    struct us_internal_callback *internal_cb = (struct us_internal_callback *) t;

    return internal_cb->loop;
}

// async (internal only)
struct us_internal_async *us_internal_create_async(struct us_loop *loop, int fallthrough, unsigned int ext_size) {
    struct us_internal_callback *cb = malloc(sizeof(struct us_internal_callback) + sizeof(uv_async_t) + ext_size);

    cb->loop = loop;
    return (struct us_internal_async *) cb;
}

void us_internal_async_close(struct us_internal_async *a) {
    struct us_internal_callback *cb = (struct us_internal_callback *) a;

    uv_async_t *uv_async = (uv_async_t *) (cb + 1);

    // always ref the async before closing it
    uv_ref((uv_handle_t *) uv_async);

    uv_async->data = cb;
    uv_close((uv_handle_t *) uv_async, close_cb_free);
}

void us_internal_async_set(struct us_internal_async *a, void (*cb)(struct us_internal_async *)) {
    struct us_internal_callback *internal_cb = (struct us_internal_callback *) a;

    internal_cb->cb = (void (*)(struct us_internal_callback *)) cb;

    uv_async_t *uv_async = (uv_async_t *) (internal_cb + 1);
    uv_async_init(internal_cb->loop->uv_loop, uv_async, async_cb);
    uv_unref((uv_handle_t *) uv_async);
    uv_async->data = internal_cb;
}

void us_internal_async_wakeup(struct us_internal_async *a) {
    struct us_internal_callback *internal_cb = (struct us_internal_callback *) a;

    uv_async_t *uv_async = (uv_async_t *) (internal_cb + 1);
    uv_async_send(uv_async);
}

#endif
