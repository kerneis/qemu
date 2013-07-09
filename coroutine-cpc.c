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


typedef struct {
    Coroutine base;
    struct cpc_continuation *cont;
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

static struct cpc_continuation *cont_alloc(unsigned size)
{
    struct cpc_continuation *r;
    if (size < 16)
        size = 16;
    r = g_malloc0(sizeof(*r) + size - 1);
    r->size = size - 1;
    return r;
}

static void cpc_continuation_free(struct cpc_continuation *c)
{
    //free(c);
}

cpc_continuation *
cpc_continuation_expand(struct cpc_continuation *c, int n)
{
    struct cpc_continuation *r;
    int size;

    printf("%s: expanding %p with size %d\n", __func__, c, n);

    if(c == NULL) {
        return cont_alloc(n + 20);
    }

    size = c->size * 2 + n;
    r = cont_alloc(size);

    memcpy(r->c, c->c, c->length);
    r->length = c->length;
    cpc_continuation_free(c);

    return r;
}


static struct cpc_continuation *cpc_invoke_continuation(struct cpc_continuation *c)
{
    cpc_function *f;
    struct cpc_continuation *orig_c;

    while(c) {
      if(c->length == 0) {
        cpc_continuation_free(c);
        return NULL;
      }

      c->length -= PTR_SIZE;
      f = *(cpc_function**)(c->c + c->length);
      orig_c = c;
      c = (*f)(c);
    }
    return orig_c;
}


static CoroutineThreadState *coroutine_get_thread_state(void)
{
    CoroutineThreadState *s = pthread_getspecific(thread_state_key);

    printf("%s: thread specific key %p\n", __func__, s);

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

#define INITIAL_SIZE 512

    co = g_malloc0(sizeof(*co));
    co->cont = cpc_continuation_expand(NULL, INITIAL_SIZE);

    printf("%s: returning coroutine %p\n", __func__, co);

    return &co->base;
}

void qemu_coroutine_delete(Coroutine *co_)
{
    CoroutineCPC *co = DO_UPCAST(CoroutineCPC, base, co_);

    printf("%s: deleting coroutine %p\n", __func__, co);

    cpc_continuation_free(co->cont);
    g_free(co);
}

struct arglist {
   void *arg  __attribute__((__aligned__(16))) ;
};

CoroutineAction qemu_coroutine_switch(Coroutine *from_, Coroutine *to_,
                                      CoroutineAction action)
{
    CoroutineCPC *from = DO_UPCAST(CoroutineCPC, base, from_);
    CoroutineCPC *to = DO_UPCAST(CoroutineCPC, base, to_);

    CoroutineThreadState *s = coroutine_get_thread_state();

    printf("%s: switching from coroutine %p to %p, action %d, thread state %p\n", __func__, from, to, action, s);

    s->current = to_;

    struct arglist *a = (struct arglist *)cpc_alloc(&to->cont, sizeof(struct arglist));
    a->arg = to_->entry_arg;

    to->cont = cpc_continuation_push(to->cont, to_->entry);
    to->cont = cpc_invoke_continuation(to->cont);

    /* we need to transfer execution to to. we then return from this function when execution
     * returns to *THIS* coroutine.
     * we return the action, as in whether the thread terminated or yielded.
     */

    if (!to->cont)
        return COROUTINE_TERMINATE;
    else
        return COROUTINE_YIELD;
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
