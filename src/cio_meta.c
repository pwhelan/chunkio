/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Chunk I/O
 *  =========
 *  Copyright 2018 Eduardo Silva <eduardo@monkey.io>
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#define _GNU_SOURCE
#include <sys/mman.h>
#include <string.h>

#include <chunkio/chunkio.h>
#include <chunkio/cio_file.h>
#include <chunkio/cio_file_st.h>
#include <chunkio/cio_stream.h>
#include <chunkio/cio_log.h>

/*
 * Metadata is an optional information stored before the content of each file
 * and can be used for different purposes. Manipulating metadata can have
 * some performance impacts depending on 'when' it's added and how often
 * is modified.
 *
 * For performance reasons, we suggest the metadata be stored before to write
 * any data to the content area, otherwise if metadata grows in terms of bytes
 * we need to move all the content data to a different position which is not
 * ideal.
 *
 * The caller might want to fix the performance penalties setting up some
 * empty metadata with specific sizes.
 */

/*
 * adjust_layout: if metadata has changed, we need to adjust the content
 * data and reference pointers.
 */
static int adjust_layout(struct cio_file *cf, size_t meta_size)
{
    int ret;
    crc_t crc;

    cio_file_st_set_meta_len(cf->map, (uint16_t) meta_size);

    /* Update checksum */
    if (cf->ctx->flags & CIO_CHECKSUM) {
        /* reset current crc since we are calculating from zero */
        cf->crc_cur = cio_crc32_init();
        cio_file_calculate_checksum(cf, &cf->crc_cur);
    }

    /* Sync changes to disk */
    cf->synced = CIO_FALSE;

    return 0;
}

int cio_meta_write(struct cio_file *cf, char *buf, size_t size)
{
    int ret;
    char *meta;
    char *cur_content_data;
    char *new_content_data;
    size_t diff;
    size_t new_size;
    size_t content_size;
    size_t content_av;
    size_t meta_av;
    void *tmp;

    /* Get metadata pointer */
    meta = cio_file_st_get_meta(cf->map);

    /* Check if meta already have some space available to overwrite */
    meta_av = cio_file_st_get_meta_len(cf->map);

    /* If there is some space available, just overwrite */
    if (meta_av >= size) {
        /* copy new metadata */
        memcpy(meta, buf, size);

        /* there are some remaining bytes, adjust.. */
        diff = meta_av - size;
        cur_content_data = cio_file_st_get_content(cf->map);
        new_content_data = meta + size;
        memmove(new_content_data, cur_content_data, cf->data_size);
        adjust_layout(cf, size);

        return 0;
    }

    /*
     * The optimal case is if there is no content data, the non-optimal case
     * where we need to increase the memory map size, move the content area
     * bytes to a different position and write the metadata.
     *
     * Calculate the available space in the content area.
     */
    content_av = cf->alloc_size - cf->data_size;

    /* If there is no enough space, increase the file size and it memory map */
    if (content_av < size) {
        new_size = (size - meta_av) + cf->data_size + CIO_FILE_HEADER_MIN;

        /* Increase memory map size */
        tmp = mremap(cf->map, cf->alloc_size, new_size, MREMAP_MAYMOVE);
        if (tmp == MAP_FAILED) {
            cio_errno();
            cio_log_error(cf->ctx,
                          "[cio meta] data exceeds available space "
                          "(alloc=%lu current_size=%lu meta_size=%lu)",
                          cf->alloc_size, cf->data_size, size);
            return -1;

        }
        cf->map = tmp;
        cf->alloc_size = new_size;

        /* Alter file size (file system) */
        ret = cio_file_fs_size_change(cf, new_size);
        if (ret == -1) {
            cio_errno();
            return -1;
        }
    }

    /* get meta reference again in case the map address has changed */
    meta = cio_file_st_get_meta(cf->map);

    /* set new position for the content data */
    cur_content_data = cio_file_st_get_content(cf->map);
    new_content_data = meta + size;
    memmove(new_content_data, cur_content_data, size);

    /* copy new metadata */
    memcpy(meta, buf, size);
    adjust_layout(cf, size);
}