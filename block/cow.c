/*
 * Block driver for the COW format
 *
 * Copyright (c) 2004 Fabrice Bellard
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
#include "qemu-common.h"
#include "block/block_int.h"
#include "qemu/module.h"

/**************************************************************/
/* COW block driver using file system holes */

/* user mode linux compatible COW file */
#define COW_MAGIC 0x4f4f4f4d  /* MOOO */
#define COW_VERSION 2

struct cow_header_v2 {
    uint32_t magic;
    uint32_t version;
    char backing_file[1024];
    int32_t mtime;
    uint64_t size;
    uint32_t sectorsize;
};

typedef struct BDRVCowState {
    CoMutex lock;
    int64_t cow_sectors_offset;
} BDRVCowState;

static int cow_probe(const uint8_t *buf, int buf_size, const char *filename)
{
    const struct cow_header_v2 *cow_header = (const void *)buf;

    if (buf_size >= sizeof(struct cow_header_v2) &&
        be32_to_cpu(cow_header->magic) == COW_MAGIC &&
        be32_to_cpu(cow_header->version) == COW_VERSION)
        return 100;
    else
        return 0;
}

static int cow_open(BlockDriverState *bs, QDict *options, int flags)
{
    BDRVCowState *s = bs->opaque;
    struct cow_header_v2 cow_header;
    int bitmap_size;
    int64_t size;
    int ret;

    /* see if it is a cow image */
    ret = bdrv_pread(bs->file, 0, &cow_header, sizeof(cow_header));
    if (ret < 0) {
        goto fail;
    }

    if (be32_to_cpu(cow_header.magic) != COW_MAGIC) {
        ret = -EMEDIUMTYPE;
        goto fail;
    }

    if (be32_to_cpu(cow_header.version) != COW_VERSION) {
        char version[64];
        snprintf(version, sizeof(version),
               "COW version %d", cow_header.version);
        qerror_report(QERR_UNKNOWN_BLOCK_FORMAT_FEATURE,
            bs->device_name, "cow", version);
        ret = -ENOTSUP;
        goto fail;
    }

    /* cow image found */
    size = be64_to_cpu(cow_header.size);
    bs->total_sectors = size / 512;

    pstrcpy(bs->backing_file, sizeof(bs->backing_file),
            cow_header.backing_file);

    bitmap_size = ((bs->total_sectors + 7) >> 3) + sizeof(cow_header);
    s->cow_sectors_offset = (bitmap_size + 511) & ~511;
    qemu_co_mutex_init(&s->lock);
    return 0;
 fail:
    return ret;
}

#define BITS_PER_BITMAP_SECTOR (BDRV_SECTOR_SIZE * 8)

/* Cannot use bitmap.c on big-endian machines.  */
static int cow_test_bit(int64_t bitnum, const uint8_t *bitmap)
{
    return (bitmap[bitnum / 8] & (1 << (bitnum & 7))) != 0;
}

static int cow_find_streak(const uint8_t *bitmap, int value, int start,
                           int nb_sectors)
{
    int streak_value = value ? 0xFF : 0;
    int last = MIN(start + nb_sectors, BITS_PER_BITMAP_SECTOR);
    int bitnum = start;
    while (bitnum < last) {
        if ((bitnum & 7) == 0 && bitmap[bitnum / 8] == streak_value) {
            bitnum += 8;
            continue;
        }
        if (cow_test_bit(bitnum, bitmap) == value) {
            bitnum++;
            continue;
        }
        break;
    }
    return MIN(bitnum, last) - start;
}

static int cow_is_allocated(const uint8_t *bitmap,
        int64_t sector_num, int nb_sectors, int *num_same)
{
    int changed;
    int64_t bitnum = sector_num + sizeof(struct cow_header_v2) * 8;

    bitnum &= BITS_PER_BITMAP_SECTOR - 1;
    changed = cow_test_bit(bitnum, bitmap);
    *num_same = cow_find_streak(bitmap, changed, bitnum, nb_sectors);
    return changed;
}

/* Return true if first block has been changed (ie. current version is
 * in COW file).  Set the number of continuous blocks for which that
 * is true. */
static int coroutine_fn cow_co_is_allocated(BlockDriverState *bs,
        int64_t sector_num, int nb_sectors, int *num_same)
{
    int ret;
    int64_t bitnum = sector_num + sizeof(struct cow_header_v2) * 8;
    uint64_t offset = (bitnum / 8) & -BDRV_SECTOR_SIZE;
    bool first = false, changed = false;
    int64_t remaining = nb_sectors;

    *num_same = 0;
    while (remaining) {
        int ns;
        uint8_t bitmap[BDRV_SECTOR_SIZE];
        bool c;

        ret = bdrv_pread(bs->file, offset, &bitmap, sizeof(bitmap));
        if (ret < 0) {
            return ret;
        }

        c = cow_is_allocated(bitmap, sector_num, nb_sectors, &ns);
        if (!first) {
            changed = c;
            first = true;
        }
        *num_same += ns;
        if (ns != BDRV_SECTOR_SIZE) {
            break;
        }
        offset += BDRV_SECTOR_SIZE;
    }

    return changed;
}

/* Update up to one sectors worth of bitmap, return number of bits processed */
static int64_t cow_update_bitmap_sector(uint8_t *buf, int64_t sector_num,
        int nb_sectors, bool *flushed)
{
    int init_bits = MIN(nb_sectors,
            (sector_num % 8) ? (8 - (sector_num % 8)) : 0);
    int remaining = MIN(nb_sectors - init_bits, BITS_PER_BITMAP_SECTOR);
    int full_bytes = remaining / 8;
    int trail = remaining % 8;

    int64_t processed = init_bits + full_bytes * 8 + trail;
    int len = !!init_bits + full_bytes + !!trail;

    /* Do sector_num -> nearest byte boundary */
    if (init_bits) {
        /* This sets the highest init_bits bits in the byte */
        uint8_t bits = ((1 << init_bits) - 1) << (8 - init_bits);
        buf[0] |= bits;
    }

    if (full_bytes) {
        memset(&buf[!!init_bits], ~0, full_bytes);
    }

    /* Set the trailing bits in the final byte */
    if (trail) {
        /* This sets the lowest trail bits in the byte */
        uint8_t bits = (1 << trail) - 1;
        buf[len - 1] |= bits;
    }

    return processed;
}

/* Set the bits from sector_num to sector_num + nb_sectors in the bitmap of
 * bs->file. */
static int cow_update_bitmap(BlockDriverState *bs, int64_t sector_num,
        int nb_sectors)
{
    bool flushed = false;
    int64_t start = sector_num, remaining = nb_sectors;
    int64_t bitnum = sector_num + sizeof(struct cow_header_v2) * 8;
    uint64_t offset = (bitnum / 8);

    while (remaining) {
        int ret;
        int64_t processed;
        uint8_t bitmap[BDRV_SECTOR_SIZE];

        ret = bdrv_pread(bs->file, offset, bitmap, sizeof(bitmap));
        if (ret < 0) {
            return ret;
        }

        processed = cow_update_bitmap_sector(bitmap, start, remaining,
                &flushed);
        if (processed < 0) {
            return processed;
        }

        start += processed;
        remaining -= processed;

        /* We need to flush the data before writing the metadata so that there
         * is no chance of metadata referring to data that doesn't exist. */
        if (!flushed) {
            ret = bdrv_flush(bs->file);
            if (ret < 0) {
                return ret;
            }
            flushed = true;
        }

        ret = bdrv_pwrite(bs->file, offset, bitmap, sizeof(bitmap));
        if (ret < 0) {
            return ret;
        }
        offset += sizeof(bitmap);
    }

    return 0;
}

static int coroutine_fn cow_read(BlockDriverState *bs, int64_t sector_num,
                                 uint8_t *buf, int nb_sectors)
{
    BDRVCowState *s = bs->opaque;
    int ret, n;

    while (nb_sectors > 0) {
        if (bdrv_co_is_allocated(bs, sector_num, nb_sectors, &n)) {
            ret = bdrv_pread(bs->file,
                        s->cow_sectors_offset + sector_num * 512,
                        buf, n * 512);
            if (ret < 0) {
                return ret;
            }
        } else {
            if (bs->backing_hd) {
                /* read from the base image */
                ret = bdrv_read(bs->backing_hd, sector_num, buf, n);
                if (ret < 0) {
                    return ret;
                }
            } else {
                memset(buf, 0, n * 512);
            }
        }
        nb_sectors -= n;
        sector_num += n;
        buf += n * 512;
    }
    return 0;
}

static coroutine_fn int cow_co_read(BlockDriverState *bs, int64_t sector_num,
                                    uint8_t *buf, int nb_sectors)
{
    int ret;
    BDRVCowState *s = bs->opaque;
    qemu_co_mutex_lock(&s->lock);
    ret = cow_read(bs, sector_num, buf, nb_sectors);
    qemu_co_mutex_unlock(&s->lock);
    return ret;
}

static int cow_write(BlockDriverState *bs, int64_t sector_num,
                     const uint8_t *buf, int nb_sectors)
{
    BDRVCowState *s = bs->opaque;
    int ret;

    ret = bdrv_pwrite(bs->file, s->cow_sectors_offset + sector_num * 512,
                      buf, nb_sectors * 512);
    if (ret < 0) {
        return ret;
    }

    return cow_update_bitmap(bs, sector_num, nb_sectors);
}

static coroutine_fn int cow_co_write(BlockDriverState *bs, int64_t sector_num,
                                     const uint8_t *buf, int nb_sectors)
{
    int ret;
    BDRVCowState *s = bs->opaque;
    qemu_co_mutex_lock(&s->lock);
    ret = cow_write(bs, sector_num, buf, nb_sectors);
    qemu_co_mutex_unlock(&s->lock);
    return ret;
}

static void cow_close(BlockDriverState *bs)
{
}

static int cow_create(const char *filename, QEMUOptionParameter *options)
{
    struct cow_header_v2 cow_header;
    struct stat st;
    int64_t image_sectors = 0;
    const char *image_filename = NULL;
    int ret;
    BlockDriverState *cow_bs;

    /* Read out options */
    while (options && options->name) {
        if (!strcmp(options->name, BLOCK_OPT_SIZE)) {
            image_sectors = options->value.n / 512;
        } else if (!strcmp(options->name, BLOCK_OPT_BACKING_FILE)) {
            image_filename = options->value.s;
        }
        options++;
    }

    ret = bdrv_create_file(filename, options);
    if (ret < 0) {
        return ret;
    }

    ret = bdrv_file_open(&cow_bs, filename, NULL, BDRV_O_RDWR);
    if (ret < 0) {
        return ret;
    }

    memset(&cow_header, 0, sizeof(cow_header));
    cow_header.magic = cpu_to_be32(COW_MAGIC);
    cow_header.version = cpu_to_be32(COW_VERSION);
    if (image_filename) {
        /* Note: if no file, we put a dummy mtime */
        cow_header.mtime = cpu_to_be32(0);

        if (stat(image_filename, &st) != 0) {
            goto mtime_fail;
        }
        cow_header.mtime = cpu_to_be32(st.st_mtime);
    mtime_fail:
        pstrcpy(cow_header.backing_file, sizeof(cow_header.backing_file),
                image_filename);
    }
    cow_header.sectorsize = cpu_to_be32(512);
    cow_header.size = cpu_to_be64(image_sectors * 512);
    ret = bdrv_pwrite(cow_bs, 0, &cow_header, sizeof(cow_header));
    if (ret < 0) {
        goto exit;
    }

    /* resize to include at least all the bitmap */
    ret = bdrv_truncate(cow_bs,
        sizeof(cow_header) + ((image_sectors + 7) >> 3));
    if (ret < 0) {
        goto exit;
    }

exit:
    bdrv_delete(cow_bs);
    return ret;
}

static QEMUOptionParameter cow_create_options[] = {
    {
        .name = BLOCK_OPT_SIZE,
        .type = OPT_SIZE,
        .help = "Virtual disk size"
    },
    {
        .name = BLOCK_OPT_BACKING_FILE,
        .type = OPT_STRING,
        .help = "File name of a base image"
    },
    { NULL }
};

static BlockDriver bdrv_cow = {
    .format_name    = "cow",
    .instance_size  = sizeof(BDRVCowState),

    .bdrv_probe     = cow_probe,
    .bdrv_open      = cow_open,
    .bdrv_close     = cow_close,
    .bdrv_create    = cow_create,
    .bdrv_has_zero_init     = bdrv_has_zero_init_1,

    .bdrv_read              = cow_co_read,
    .bdrv_write             = cow_co_write,
    .bdrv_co_is_allocated   = cow_co_is_allocated,

    .create_options = cow_create_options,
};

static void bdrv_cow_init(void)
{
    bdrv_register(&bdrv_cow);
}

block_init(bdrv_cow_init);
