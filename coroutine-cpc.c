/*
Copyright (c) 2008-2011,
  Gabriel Kerneis     <kerneis@pps.univ-paris-diderot.fr>
Copyright (c) 2004-2005,
  Juliusz Chroboczek  <jch@pps.univ-paris-diderot.fr>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#define NO_CPS_PROTO
#include "cpc/cpc_runtime.h"

#include "block/coroutine_int.h"

#include <pthread.h>

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <fcntl.h>
#include <stddef.h>

#define STATE_UNKNOWN -1
#define STATE_SLEEPING -2
#define STATE_DETACHED -3

typedef struct {
    Coroutine base;
} CoroutineCPC;

/**
 * Per-thread coroutine bookkeeping
 */
typedef struct {
    /** Currently executing coroutine */
    Coroutine *current;

    /** The default coroutine */
    CoroutineCPC leader;
} CoroutineThreadState;

static pthread_key_t thread_state_key;

typedef struct cpc_thread {
    struct cpc_thread *next;
    struct cpc_condvar *condvar;
    struct cpc_thread *cond_next;
    cpc_sched *sched;
    int state;
    struct cpc_continuation cont;
} cpc_thread;

struct cpc_thread *
cpc_thread_get(int size)
{
    struct cpc_thread *t;
    t = malloc(sizeof(struct cpc_thread) + (size - 1));
    t->cont.size = size;
    return t;
}

static inline cpc_continuation *
get_cont(cpc_thread *t)
{
    return (cpc_continuation *)(((char *)t) + offsetof(struct cpc_thread, cont));
}

static inline cpc_thread *
get_thread(cpc_continuation *c)
{
    return (cpc_thread *)(((char *)c) - offsetof(struct cpc_thread, cont));
}

cpc_continuation *
cpc_continuation_expand(struct cpc_continuation *c, int n)
{
    printf("%s: expanding %p with size %d\n", __func__, c, n);

    int size;
    cpc_thread *d, *t;

    if(c == (void*)0)
        size = n + 20;
    else
        size = c->size * 2 + n;

    d = cpc_thread_get(size);

    if(c == (void*)0) {
        d->cont.length = 0;
        d->condvar = NULL;
        d->cond_next = NULL;
        d->next = NULL;
        d->sched = cpc_default_sched;
        d->state = STATE_UNKNOWN;
        return get_cont(d);
    }

    t = get_thread(c);

    memcpy(d->cont.c, c->c, c->length);

    d->cont.length = c->length;
    d->condvar = t->condvar;
    d->cond_next = t->cond_next;
    d->next = t->next;
    d->sched = t->sched;
    d->state = t->state;

    free(t);

    return get_cont(d);
}


static CoroutineThreadState *coroutine_get_thread_state(void)
{
    CoroutineThreadState *s = pthread_getspecific(thread_state_key);

    printf("%s: thread specific key\n", __func__, s);

    if (!s) {
        s = g_malloc0(sizeof(*s));
        s->current = &s->leader.base;
        pthread_setspecific(thread_state_key, s);
    }
    return s;
}

Coroutine *qemu_coroutine_new(void)
{
    CoroutineCPC *co;

    co = g_malloc0(sizeof(*co));

    printf("%s: returning coroutine %p\n", __func__, co);

    return &co->base;
}

void qemu_coroutine_delete(Coroutine *co_)
{
    CoroutineCPC *co = DO_UPCAST(CoroutineCPC, base, co_);

    printf("%s: deleting coroutine %p\n", __func__, co);

    /* g_free(co->stack); */
    g_free(co);
}

CoroutineAction qemu_coroutine_switch(Coroutine *from_, Coroutine *to_,
                                      CoroutineAction action)
{
    CoroutineCPC *from = DO_UPCAST(CoroutineCPC, base, from_);
    CoroutineCPC *to = DO_UPCAST(CoroutineCPC, base, to_);

    CoroutineThreadState *s = coroutine_get_thread_state();

    printf("%s: switching from coroutine %p to %p, action %d, thread state %p\n", __func__, from, to, action, s);

    s->current = to_;

    /* we need to transfer execution to to. we then return from this function when execution
     * returns to *THIS* coroutine.
     * we return the action, as in whether the thread terminated or yielded.
     */

    return 0;
}

Coroutine *qemu_coroutine_self(void)
{
    CoroutineThreadState *s = coroutine_get_thread_state();

    printf("%s: returning current coroutine %p\n", __func__, s);

    return s->current;
}

bool qemu_in_coroutine(void)
{
    CoroutineThreadState *s = pthread_getspecific(thread_state_key);

    printf("%s: returning current coroutine %p, caller %p\n", __func__, s, s ? s->current->caller : NULL);

    return s && s->current->caller;
}
