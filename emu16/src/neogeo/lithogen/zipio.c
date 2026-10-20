/* SPDX-License-Identifier: BSD-3-Clause
 * liblithogen - zip archive input (miniz backend).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "../miniz/miniz.h"
#include "lithogen_internal.h"

struct nc_zipset {
    mz_zip_archive *zips;
    size_t n;
};

static int name_eq(const char *entry, const char *want)
{
    /* compare basename of the entry, case-insensitively */
    const char *base = entry;
    const char *p;
    for (p = entry; *p; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    for (; *base && *want; base++, want++)
        if (tolower((unsigned char)*base) != tolower((unsigned char)*want))
            return 0;
    return *base == '\0' && *want == '\0';
}

nc_zipset *nc_zipset_open(const char *const *paths, size_t npaths, char *err)
{
    nc_zipset *zs = (nc_zipset *)calloc(1, sizeof(*zs));
    size_t i;
    if (!zs)
        return NULL;
    zs->zips = (mz_zip_archive *)calloc(npaths, sizeof(mz_zip_archive));
    if (!zs->zips) {
        free(zs);
        return NULL;
    }
    for (i = 0; i < npaths; i++) {
        if (!mz_zip_reader_init_file(&zs->zips[i], paths[i], 0)) {
            snprintf(err, LITHOGEN_ERRSTR_MAX, "cannot open zip: %s", paths[i]);
            zs->n = i;
            nc_zipset_close(zs);
            return NULL;
        }
    }
    zs->n = npaths;
    return zs;
}

void nc_zipset_close(nc_zipset *zs)
{
    size_t i;
    if (!zs)
        return;
    for (i = 0; i < zs->n; i++)
        mz_zip_reader_end(&zs->zips[i]);
    free(zs->zips);
    free(zs);
}

static uint8_t *extract(mz_zip_archive *za, mz_uint idx, size_t *out_len,
                        uint32_t *out_crc)
{
    mz_zip_archive_file_stat st;
    uint8_t *buf;
    if (!mz_zip_reader_file_stat(za, idx, &st))
        return NULL;
    buf = (uint8_t *)malloc(st.m_uncomp_size ? (size_t)st.m_uncomp_size : 1);
    if (!buf)
        return NULL;
    if (!mz_zip_reader_extract_to_mem(za, idx, buf, (size_t)st.m_uncomp_size, 0)) {
        free(buf);
        return NULL;
    }
    *out_len = (size_t)st.m_uncomp_size;
    *out_crc = (uint32_t)st.m_crc32;
    return buf;
}

/* emu16: nc_zipset_read's lookup, shared with nc_zipset_stream */
static int find(nc_zipset *zs, const char *name, uint32_t crc,
                size_t expect_len, mz_zip_archive **out_za, mz_uint *out_idx)
{
    size_t z;
    mz_uint i;
    /* pass 1: by name */
    for (z = 0; z < zs->n; z++) {
        mz_zip_archive *za = &zs->zips[z];
        mz_uint nfiles = mz_zip_reader_get_num_files(za);
        for (i = 0; i < nfiles; i++) {
            char fname[512];
            mz_zip_reader_get_filename(za, i, fname, sizeof(fname));
            if (name_eq(fname, name)) {
                *out_za = za;
                *out_idx = i;
                return 1;
            }
        }
    }
    /* pass 2: by CRC32 (renamed files, split/merged set differences) */
    if (crc) {
        for (z = 0; z < zs->n; z++) {
            mz_zip_archive *za = &zs->zips[z];
            mz_uint nfiles = mz_zip_reader_get_num_files(za);
            for (i = 0; i < nfiles; i++) {
                mz_zip_archive_file_stat st;
                if (!mz_zip_reader_file_stat(za, i, &st))
                    continue;
                /* >=, not ==: expect_len is the FIRST load chunk, and a file
                 * loaded as LOAD + CONTINUE (mslug 201-p1, 2 MB in two 1 MB
                 * chunks) is longer than it.  Under == the CRC pass could never
                 * match a renamed multi-chunk file, so a MAME 0.139 set whose
                 * 201-p1.bin is byte-identical to 201-p1.p1 was "missing". */
                if ((uint32_t)st.m_crc32 == crc &&
                    (size_t)st.m_uncomp_size >= expect_len) {
                    *out_za = za;
                    *out_idx = i;
                    return 1;
                }
            }
        }
    }
    return 0;
}

uint8_t *nc_zipset_read(nc_zipset *zs, const char *name, uint32_t crc,
                        size_t expect_len, size_t *out_len, uint32_t *out_crc)
{
    mz_zip_archive *za;
    mz_uint i;
    if (!find(zs, name, crc, expect_len, &za, &i))
        return NULL;
    return extract(za, i, out_len, out_crc);
}

nc_zipstream *nc_zipset_stream(nc_zipset *zs, const char *name, uint32_t crc,
                               size_t expect_len, size_t *out_len,
                               uint32_t *out_crc)
{
    mz_zip_archive *za;
    mz_uint i;
    mz_zip_archive_file_stat st;
    if (!find(zs, name, crc, expect_len, &za, &i) ||
        !mz_zip_reader_file_stat(za, i, &st))
        return NULL;
    *out_len = (size_t)st.m_uncomp_size;
    *out_crc = (uint32_t)st.m_crc32;
    return (nc_zipstream *)mz_zip_reader_extract_iter_new(za, i, 0);
}

size_t nc_zipstream_read(nc_zipstream *s, void *buf, size_t n)
{
    return mz_zip_reader_extract_iter_read(
        (mz_zip_reader_extract_iter_state *)s, buf, n);
}

int nc_zipstream_close(nc_zipstream *s)
{
    return s ? (int)mz_zip_reader_extract_iter_free(
        (mz_zip_reader_extract_iter_state *)s) : 1;
}
