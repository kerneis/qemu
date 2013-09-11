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


#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <fcntl.h>
#include <stddef.h>

#define QEMU_COROUTINE_CPC
#include "cpc/cpc_runtime.h"
#include "block/coroutine_int.h"



typedef struct {
    Coroutine base;
    struct cpc_continuation *cont;
} CoroutineCPC;

static struct cpc_continuation *cont_alloc(unsigned size)
{
    struct cpc_continuation *r;
    if (size < 16) {
        size = 16;
    }
    r = g_malloc0(sizeof(*r) + size - 1);
    r->size = size - 1;
    return r;
}

static void cpc_continuation_free(struct cpc_continuation *c)
{
    g_free(c);
}

cpc_continuation *
cpc_continuation_expand(struct cpc_continuation *c, int n)
{
    struct cpc_continuation *r;
    int size;

    if (c == NULL) {
        return cont_alloc(n + 20);
    }

    size = c->size * 2 + n;
    r = cont_alloc(size);

    memcpy(r->c, c->c, c->length);
    r->length = c->length;
    r->coroutine = c->coroutine;
    cpc_continuation_free(c);

    return r;
}


static struct cpc_continuation *
cpc_invoke_continuation(struct cpc_continuation *c)
{
    cpc_function *f = NULL;
    const cpc_function *yield_func = qemu_coroutine_yield;

    while (1) {
        /* If the continuation is empty, free it and return NULL, signalling
         * this continuation terminated. */
        if (c->length == 0) {
            cpc_continuation_free(c);
            return NULL;
        }

        /* Extract the next function from the continuation. */
        c->length -= PTR_SIZE;
        f = *(cpc_function **)(c->c + c->length);

        /* If we need to yield, return immediately.  This hack is necessary to
         * avoid modifying the implementation of qemu_coroutine_yield.  */
        if (f == yield_func) {
            return c;
        }
        c = (*f)(c);
    }
}

Coroutine *qemu_coroutine_new(void)
{
    CoroutineCPC *co;

    co = g_malloc0(sizeof(*co));
    co->cont = NULL;

    return &co->base;
}

void qemu_coroutine_delete(Coroutine *co_)
{
    CoroutineCPC *co = DO_UPCAST(CoroutineCPC, base, co_);

    cpc_continuation_free(co->cont);
    g_free(co);
}

struct arglist {
   void *arg  __attribute__((__aligned__)) ;
};

CoroutineAction qemu_coroutine_switch(Coroutine *from_, Coroutine *to_,
                                      CoroutineAction action)
{
    CoroutineCPC *to = DO_UPCAST(CoroutineCPC, base, to_);

    if (!to->cont) {
#define INITIAL_SIZE 512
        to->cont = cpc_continuation_expand(NULL, INITIAL_SIZE);
        struct arglist *a = cpc_alloc(&to->cont, sizeof(struct arglist));
        a->arg = to_->entry_arg;
        to->cont = cpc_continuation_push(to->cont, to_->entry);
    }

    to->cont->coroutine = to_;

    to->cont = cpc_invoke_continuation(to->cont);

    if (!to->cont) {
        return COROUTINE_TERMINATE;
    } else {
        /* Fix the caller. This is normally done in qemu_coroutine_yield
         * but we bypass it for CPC. */
        to->cont->coroutine = to_->caller;
        to_->caller = NULL;
        return COROUTINE_YIELD;
    }
}

Coroutine *qemu_coroutine_self_int(cpc_continuation *c)
{
    return (c ? c->coroutine : NULL);
}

Coroutine *qemu_in_coroutine(cpc_continuation *c)
{
    return (c != NULL);
}
