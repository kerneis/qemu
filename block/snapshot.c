/*
 * Block layer snapshot related functions
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "block/snapshot.h"
#include "block/block_int.h"

#define NOT_DONE 0x7fffffff

int bdrv_snapshot_find(BlockDriverState *bs, QEMUSnapshotInfo *sn_info,
                       const char *name)
{
    QEMUSnapshotInfo *sn_tab, *sn;
    int nb_sns, i, ret;

    ret = -ENOENT;
    nb_sns = bdrv_snapshot_list(bs, &sn_tab);
    if (nb_sns < 0) {
        return ret;
    }
    for (i = 0; i < nb_sns; i++) {
        sn = &sn_tab[i];
        if (!strcmp(sn->id_str, name) || !strcmp(sn->name, name)) {
            *sn_info = *sn;
            ret = 0;
            break;
        }
    }
    g_free(sn_tab);
    return ret;
}

int bdrv_can_snapshot(BlockDriverState *bs)
{
    BlockDriver *drv = bs->drv;
    if (!drv || !bdrv_is_inserted(bs) || bdrv_is_read_only(bs)) {
        return 0;
    }

    if (!drv->bdrv_snapshot_create) {
        if (bs->file != NULL) {
            return bdrv_can_snapshot(bs->file);
        }
        return 0;
    }

    return 1;
}

int bdrv_snapshot_create(BlockDriverState *bs,
                         QEMUSnapshotInfo *sn_info)
{
    BlockDriver *drv = bs->drv;
    if (!drv) {
        return -ENOMEDIUM;
    }
    if (drv->bdrv_snapshot_create) {
        return drv->bdrv_snapshot_create(bs, sn_info);
    }
    if (bs->file) {
        return bdrv_snapshot_create(bs->file, sn_info);
    }
    return -ENOTSUP;
}

struct SnapOp {
    BlockDriverState *bs;
    int ret;
};

static void coroutine_fn bdrv_snapshot_open_entry(void *opaque)
{
    struct SnapOp *so = opaque;
    so->ret = so->bs->drv->bdrv_co_open(so->bs, NULL, so->bs->open_flags);
}

static int bdrv_snapshot_open(BlockDriverState *bs)
{
    Coroutine *co;
    struct SnapOp so = {
        .bs = bs,
        .ret = NOT_DONE,
    };

    co = qemu_coroutine_create(bdrv_snapshot_open_entry);
    qemu_coroutine_enter(co, &so);
    while (so.ret == NOT_DONE) {
        qemu_aio_wait();
    }

    return so.ret;
}


typedef struct BCCo {
    BlockDriverState *bs;
    bool done;
} BCCo;


static void coroutine_fn b_close_co_entry(void *opaque)
{
    BCCo *bcco = opaque;
    BlockDriver *drv = bcco->bs->drv;

    drv->bdrv_close(bcco->bs);
    bcco->done = true;
}

static void b_close(BlockDriverState *bs)
{
    BCCo bcco = {
        .bs = bs,
        .done = false,
    };
    Coroutine *co;

    co = qemu_coroutine_create(b_close_co_entry);
    qemu_coroutine_enter(co, &bcco);
    while (bcco.done) {
        qemu_aio_wait();
    }
}


int bdrv_snapshot_goto(BlockDriverState *bs,
                       const char *snapshot_id)
{
    BlockDriver *drv = bs->drv;
    int ret, open_ret;

    if (!drv) {
        return -ENOMEDIUM;
    }
    if (drv->bdrv_snapshot_goto) {
        return drv->bdrv_snapshot_goto(bs, snapshot_id);
    }

    if (bs->file) {
        b_close(bs);
        ret = bdrv_snapshot_goto(bs->file, snapshot_id);
        open_ret = bdrv_snapshot_open(bs);
        if (open_ret < 0) {
            bdrv_sync_delete(bs->file);
            bs->drv = NULL;
            return open_ret;
        }
        return ret;
    }

    return -ENOTSUP;
}

int bdrv_snapshot_delete(BlockDriverState *bs, const char *snapshot_id)
{
    BlockDriver *drv = bs->drv;
    if (!drv) {
        return -ENOMEDIUM;
    }
    if (drv->bdrv_snapshot_delete) {
        return drv->bdrv_snapshot_delete(bs, snapshot_id);
    }
    if (bs->file) {
        return bdrv_snapshot_delete(bs->file, snapshot_id);
    }
    return -ENOTSUP;
}

int bdrv_snapshot_list(BlockDriverState *bs,
                       QEMUSnapshotInfo **psn_info)
{
    BlockDriver *drv = bs->drv;
    if (!drv) {
        return -ENOMEDIUM;
    }
    if (drv->bdrv_snapshot_list) {
        return drv->bdrv_snapshot_list(bs, psn_info);
    }
    if (bs->file) {
        return bdrv_snapshot_list(bs->file, psn_info);
    }
    return -ENOTSUP;
}

int bdrv_snapshot_load_tmp(BlockDriverState *bs,
        const char *snapshot_name)
{
    BlockDriver *drv = bs->drv;
    if (!drv) {
        return -ENOMEDIUM;
    }
    if (!bs->read_only) {
        return -EINVAL;
    }
    if (drv->bdrv_snapshot_load_tmp) {
        return drv->bdrv_snapshot_load_tmp(bs, snapshot_name);
    }
    return -ENOTSUP;
}
