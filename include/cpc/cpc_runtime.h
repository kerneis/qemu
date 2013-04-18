/*
Copyright (c) 2008-2011,
  Gabriel Kerneis     <kerneis@pps.jussieu.fr>
Copyright (c) 2004-2005,
  Juliusz Chroboczek  <jch@pps.jussieu.fr>

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

/* CPC cannot parse anonymous functions (aka Apple's "blocks") */
#undef __BLOCKS__

#include <stddef.h> // size_t
#include <time.h>

/* If you want to build with the --packed option, do not forget to set:
#define CPC_COMPACT_CONTINUATIONS 1
This is broken on some architectures.
*/

/* If you want to build with the --ecpc option, do not forget to set:
#define CPC_INDIRECT_PATCH 1
*/

#ifdef CPC_COMPACT_CONTINUATIONS

#define MAX_ALIGN 1
#define PTR_SIZE (sizeof(cpc_function *))

#else

#ifdef __BIGGEST_ALIGNMENT__
#define MAX_ALIGN __BIGGEST_ALIGNMENT__
#else
#define MAX_ALIGN __alignof(struct {char c; } __attribute__((__aligned__)))
#endif
#define PTR_SIZE MAX_ALIGN

#endif

typedef struct cpc_condvar cpc_condvar;
typedef struct cpc_sched cpc_sched;
extern cpc_sched *cpc_default_threadpool;
#define cpc_default_sched NULL

typedef struct cpc_continuation {
    unsigned short length;
    unsigned short size;
#ifdef CPC_INDIRECT_PATCH
    void *cpc_retval; // where to write the next return value
#endif
    char c[1];
} cpc_continuation;

extern void cpc_print_continuation(struct cpc_continuation *c, char *s);

typedef cpc_continuation *cpc_function(void*);

struct cpc_continuation *cpc_continuation_expand(struct cpc_continuation *c,
                                                 int n);

static inline void* 
cpc_alloc(struct cpc_continuation **cp, int s)
{
    struct cpc_continuation *c;
    void *p;

    c = *cp;
    if(c == (void*)0 || s > c->size - c->length)
        c = cpc_continuation_expand(c, s);
    p = c->c + c->length;
    c->length += s;
    *cp = c;
    return p;
}

static inline void*
cpc_dealloc(struct cpc_continuation *c, int s)
{
    c->length -= s;
    return c->c + c->length;
}

#define CPC_IO_IN 1
#define CPC_IO_OUT 2
#define CPC_TIMEOUT 4
#define CPC_CONDVAR 8

static inline struct cpc_continuation *
cpc_continuation_push(cpc_continuation *c, cpc_function *f)
{

    if(c == (void*)0 || PTR_SIZE > c->size - c->length)
        c = cpc_continuation_expand(c, PTR_SIZE);

    *(cpc_function**)(c->c + c->length) = f;
    c->length += PTR_SIZE;
    return c;
}

static inline void
cpc_continuation_patch(cpc_continuation *cont, size_t size, const void *value)
{
  void *cpc_arg;
  cpc_arg =
#ifdef CPC_INDIRECT_PATCH
    ((cont)->cpc_retval);
  if(cpc_arg == NULL) return; /* this should not happen if the caller is smart enough */
#else
    ((cont)->c + (cont)->length - PTR_SIZE - ((size - 1) / MAX_ALIGN + 1) * MAX_ALIGN);
#endif
  __builtin_memcpy(cpc_arg, value, size);
  return;
}

