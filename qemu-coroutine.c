/*
 * QEMU coroutines
 *
 * Copyright IBM, Corp. 2011
 *
 * Authors:
 *  Stefan Hajnoczi    <stefanha@linux.vnet.ibm.com>
 *  Kevin Wolf         <kwolf@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#include "trace.h"
#include "qemu-common.h"
#include "qemu/thread.h"
#include "block/coroutine.h"
#include "block/coroutine_int.h"

#ifndef NO_COROUTINE_POOL
enum {
    /* Maximum free pool size prevents holding too many freed coroutines */
    POOL_MAX_SIZE = 64,
};

/** Free list to speed up creation */
static QemuMutex pool_lock;
static QSLIST_HEAD(, Coroutine) pool = QSLIST_HEAD_INITIALIZER(pool);
static unsigned int pool_size;
#endif

Coroutine *qemu_coroutine_create(CoroutineEntry *entry)
{
    Coroutine *co;

#ifndef NO_COROUTINE_POOL
    qemu_mutex_lock(&pool_lock);
    co = QSLIST_FIRST(&pool);
    if (co) {
        QSLIST_REMOVE_HEAD(&pool, pool_next);
        pool_size--;
    }
    qemu_mutex_unlock(&pool_lock);

    if (!co) {
        co = qemu_coroutine_new();
    }
#else
    co = qemu_coroutine_new();
#endif

    co->entry = entry;
    QTAILQ_INIT(&co->co_queue_wakeup);
    return co;
}

static void coroutine_delete(Coroutine *co)
{
#ifndef NO_COROUTINE_POOL
    qemu_mutex_lock(&pool_lock);
    if (pool_size < POOL_MAX_SIZE) {
        QSLIST_INSERT_HEAD(&pool, co, pool_next);
        co->caller = NULL;
        pool_size++;
        qemu_mutex_unlock(&pool_lock);
        return;
    }
    qemu_mutex_unlock(&pool_lock);
#endif

    qemu_coroutine_delete(co);
}

#ifndef NO_COROUTINE_POOL
static void __attribute__((constructor)) coroutine_pool_init(void)
{
    qemu_mutex_init(&pool_lock);
}

static void __attribute__((destructor)) coroutine_pool_cleanup(void)
{
    Coroutine *co;
    Coroutine *tmp;

    QSLIST_FOREACH_SAFE(co, &pool, pool_next, tmp) {
        QSLIST_REMOVE_HEAD(&pool, pool_next);
        qemu_coroutine_delete(co);
    }

    qemu_mutex_destroy(&pool_lock);
}
#endif

static void coroutine_swap(Coroutine *from, Coroutine *to)
{
    CoroutineAction ret;

    ret = qemu_coroutine_switch(from, to, COROUTINE_YIELD);

    qemu_co_queue_run_restart(to);

    switch (ret) {
    case COROUTINE_YIELD:
        return;
    case COROUTINE_TERMINATE:
        trace_qemu_coroutine_terminate(to);
        coroutine_delete(to);
        return;
    default:
        abort();
    }
}

void qemu_coroutine_enter(Coroutine *co, void *opaque)
{
  /*
    Coroutine *self = qemu_coroutine_self_int();

    trace_qemu_coroutine_enter(self, co, opaque);

    if (co->caller) {
        fprintf(stderr, "Co-routine re-entered recursively\n");
        abort();
    }

    co->caller = self;
  */
    co->caller = NULL;
    co->entry_arg = opaque;
    coroutine_swap(NULL, co);
}

void coroutine_fn qemu_coroutine_yield(void)
{
    Coroutine *self = qemu_coroutine_self();
    Coroutine *to = self->caller;

    trace_qemu_coroutine_yield(self, to);

    if (!to) {
        fprintf(stderr, "Co-routine is yielding to no one\n");
        abort();
    }

    self->caller = NULL;
    coroutine_swap(self, to);
}

Coroutine *coroutine_fn qemu_coroutine_self(void)
{
    /* Call the internal version of this function, which is
     * non-coroutine_fn and can therefore be called from from
     * non-coroutine contexts.  Internally we know it's always possible
     * to pull a Coroutine* out of thin air (or thread-local storage).
     * External callers shouldn't assume they can always get a
     * Coroutine* since we may not be in coroutine context, hence the
     * external version of this function.
     */
    return qemu_coroutine_self_int();
}
