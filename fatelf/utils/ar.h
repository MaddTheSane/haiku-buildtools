/*
 * Copyright 2012-2013, Landon Fuller <landonf@bikemonkey.org>.
 * All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

// Standard AR definitions.

#include <unistd.h>

#define ARMAG       "!<arch>\n"     /* ar file magic */
#define SARMAG      8

#define AR_EFMT1    "#1/"           /* BSD extended filename format */
#define SAR_EFMT1   3

#define ARFMAG      "`\n"           /* AR entry terminator */
#define SARFMAG     3

struct ar_hdr {
    char ar_name[16];               /* name */
    char ar_date[12];               /* decimal seconds since epoch */
    char ar_uid[6];                 /* uid number */
    char ar_gid[6];                 /* gid number */
    char ar_mode[8];                /* octal file mode */
    char ar_size[10];               /* file size in bytes */
    char ar_fmag[2];                /* consistency check */
};

typedef struct ar_entry {
    struct ar_hdr hdr;   /* ar header */

    char    *name;       /* name */
    time_t  date;        /* seconds since epoch */
    uid_t   uid;         /* uid number */
    gid_t   gid;         /* gid number */
    mode_t  mode;        /* file mode */
    off_t   size;        /* file size */
    off_t   offset;      /* offset to data, from the start of the file. */
} AR_FILE;

typedef struct ar AR;

// open ab ar file
AR *ar_open (const char *fname);

// read the next AR record
AR_FILE *ar_read (AR *ar);

// return the read-only fd
int ar_fd (AR *ar);

// close the ar file and release all resources
void ar_close (AR *ar);


