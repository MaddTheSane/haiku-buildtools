/*
 * Copyright 2012-2013, Landon Fuller <landonf@bikemonkey.org>.
 * All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

// Standard AR definitions.

#define ARMAG       "!<arch>\n"     /* ar file magic */
#define SARMAG      8               /* ar file magic size */

#define AR_EFMT1    "#1/"           /* BSD extended filename format */
#define ARFMAG      "`\n"           /* AR entry terminator */

struct ar_hdr {
    char ar_name[16];               /* name */
    char ar_date[12];               /* decimal seconds since epoch */
    char ar_uid[6];                 /* uid number */
    char ar_gid[6];                 /* gid number */
    char ar_mode[8];                /* octal file mode */
    char ar_size[10];               /* file size in bytes */
    char ar_fmag[2];                /* consistency check */
};
