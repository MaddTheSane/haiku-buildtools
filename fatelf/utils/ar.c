/*
 * Copyright 2012-2013, Landon Fuller <landonf@bikemonkey.org>.
 * All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

#define FATELF_UTILS 1
#include "fatelf-utils.h"
#include "ar.h"

#include <stdint.h>
#include <inttypes.h>
#include <ctype.h>

struct ar {
    int fd;                     // open fd
    char *fname;                // archive file name
    struct ar_entry ar_entry;   // last-read entry

    char *string_table;         // GNU string table
    size_t string_table_size;   // table size in bytes

};

AR *ar_open (const char *fname) {
    AR *res = xmalloc(sizeof(AR));
    uint8_t magic[SARMAG];

    res->fname = xstrdup(fname);
    res->fd = xopen(fname, O_RDONLY, 0600);
    res->string_table = NULL;
    res->string_table_size = 0;

    res->ar_entry.name = NULL;
    res->ar_entry.offset = 0;

    xread(res->fname, res->fd, magic, SARMAG, true);
    if (memcmp(magic, ARMAG, SARMAG) != 0)
        xfail("'%s' is not a valid ar archive\n", fname);

    return res;
}

void ar_close (AR *ar) {
    xclose(ar->fname, ar->fd);

    if (ar->fname != NULL)
        free(ar->fname);

    if (ar->string_table != NULL)
        free(ar->string_table);

    if (ar->ar_entry.name != NULL)
        free(ar->ar_entry.name);
}

int ar_fd (AR *ar) {
    return ar->fd;
}

AR_FILE *ar_read (AR *ar) {
    struct ar_entry *ar_entry = &ar->ar_entry;
    struct ar_hdr *ar_hdr = &ar->ar_entry.hdr;
    unsigned int ar_uint;
    uint64_t ar_uint64;
    int i;

    // Read the next header
    if (ar_entry->offset != 0)
        xlseek(ar->fname, ar->fd, ar_entry->offset + ar_entry->size, SEEK_SET);

    if (xread(ar->fname, ar->fd, ar_hdr, sizeof(*ar_hdr), 0) != sizeof(*ar_hdr))
        return NULL;

    if (memcmp(ar_hdr->ar_fmag, ARFMAG, SARFMAG) != 0)
        xfail("Read invalid ar_fmag magic in '%s'", ar->fname);

    // Save the current position for later seeking
    ar_entry->offset = xlseek(ar->fname, ar->fd, 0, SEEK_CUR);

    // Extract the file metadata
    sscanf(ar_hdr->ar_date, "%" PRIu64, &ar_uint64);
    ar_entry->date = ar_uint64;

    sscanf(ar_hdr->ar_uid, "%" PRIu64, &ar_uint64);
    ar_entry->uid = ar_uint64;

    sscanf(ar_hdr->ar_gid, "%" PRIu64, &ar_uint64);
    ar_entry->gid = ar_uint64;

    sscanf(ar_hdr->ar_mode, "%o", &ar_uint);
    ar_entry->mode = ar_uint;

    sscanf(ar_hdr->ar_size, "%" PRIu64, &ar_uint64);
    ar_entry->size = ar_uint64;

    // Extract the file name
    if (ar_entry->name != NULL)
        free(ar_entry->name);

    ar_entry->name = xmalloc(sizeof(ar_hdr->ar_name) + 1);
    strncpy(ar_entry->name, ar_hdr->ar_name, sizeof(ar_hdr->ar_name));
    ar_entry->name[sizeof(ar_hdr->ar_name)] = '\0';

    for (i = sizeof(ar_hdr->ar_name); i > 0; i--) {
        char c = ar_hdr->ar_name[i - 1];

        // Names are right-padded
        if (c == ' ')
            continue;

        // GNU /-terminated name extension. We intentionally
        // don't strip the name of the '/' and '//' special files.
        if (c == '/' && i > 1 && ar_entry->name[0] != '/') {
            // end of string found
            ar_entry->name[i-1] = '\0';
            break;
        }

        // Not in GNU format
        ar_entry->name[i] = '\0';
        break;
    }

    // Handle GNU/BSD long file name extensions
    if (strncmp(ar_entry->name, AR_EFMT1, SAR_EFMT1) == 0) {
        // File name stored in BSD format, with the actual
        // name stored directly after the AR header.
        long name_size;
        sscanf(ar_entry->name + SAR_EFMT1, "%ld", &name_size);

        free(ar_entry->name);
        ar_entry->name = xmalloc(name_size+1);

        xread(ar->fname, ar->fd, ar_entry->name, name_size, 1);
        ar_entry->name[name_size] = '\0';

        // Set to the actual file position and size
        ar_entry->size -= name_size;
        ar_entry->offset += name_size;
    } else if (ar_entry->name[0] == '/' && isdigit(ar_entry->name[1]) &&
               ar->string_table != NULL)
    {
        // File name stored in GNU extension format
        long table_offset;
        size_t name_max = 0;
        size_t name_len = 0;
        char *table_entry;
        char *src;

        sscanf(ar_entry->name + 1, "%ld", &table_offset);

        name_max = ar->string_table_size - table_offset;
        if (table_offset >= ar->string_table_size)
            xfail("ar archive '%s' entry '%s' references invalid GNU string table offset", ar->fname, ar_entry->name);

        free(ar_entry->name);

        table_entry = ar->string_table + table_offset;

        // Compute the name length
        for (src = table_entry; *src != '/' && name_len < name_max; src++)
            name_len++;

        ar_entry->name = xmalloc(name_len + 1);
        strncpy(ar_entry->name, table_entry, name_len);
        ar_entry->name[name_len] = '\0';
    }

    // Handle GNU's name table / symbol files.
    if (strcmp(ar_entry->name, "//") == 0) {
        /* GNU file name table */
        if (ar->string_table != NULL)
            free(ar->string_table);

        ar->string_table = xmalloc(ar_entry->size);
        ar->string_table_size = ar_entry->size;

        xlseek(ar->fname, ar->fd, ar_entry->offset, SEEK_SET);
        xread(ar->fname, ar->fd, ar->string_table, ar_entry->size, 1);
    } else if (strcmp(ar_entry->name, "/") == 0) {
        // GNU symbol file
        // TODO - anything necessary?
    }

    return ar_entry;
}
