/**
 * FatELF; support multiple ELF binaries in one file.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

#define FATELF_UTILS 1
#include "fatelf-utils.h"
#include "fatelf-haiku.h"

static int fatelf_glue(const char *out, const char **bins, const int bincount)
{
    int i = 0;
    const size_t struct_size = fatelf_header_size(bincount);
    FATELF_header *header = (FATELF_header *) xmalloc(struct_size);
    const int outfd = xopen(out, O_RDWR | O_CREAT | O_TRUNC, 0755);
    uint64_t offset = FATELF_DISK_FORMAT_SIZE(bincount);

    unlink_on_xfail = out;

    if (bincount == 0)
        xfail("Nothing to do.");
    else if (bincount > 0xFF)
        xfail("Too many binaries (max is 255).");

    // pad out some bytes for the header we'll write at the end...
    xwrite_zeros(out, outfd, (size_t) offset);

    header->magic = FATELF_MAGIC;
    header->version = FATELF_FORMAT_VERSION;
    header->num_records = bincount;

    struct {
        int idx;
        uint64_t offset;
        uint64_t size;
    } resource;

    resource.idx = -1;
    for (i = 0; i < bincount; i++)
    {
        int j = 0;
        const uint64_t binary_offset = align_to_page(offset);
        const char *fname = bins[i];
        const int fd = xopen(fname, O_RDONLY, 0755);
        FATELF_record *record = &header->records[i];

        xread_elf_header(fname, fd, 0, record);
        record->offset = binary_offset;

        // make sure we don't have a duplicate target.
        for (j = 0; j < i; j++)
        {
            if (fatelf_record_matches(record, &header->records[j]))
                xfail("'%s' and '%s' are for the same target.", bins[j], fname);
        } // for

        // append this binary to the final file, padded to page alignment.
        xwrite_zeros(out, outfd, (size_t) (binary_offset - offset));

        // detect and skip Haiku resource data
        if (haiku_find_rsrc(fname, fd, &resource.offset, &resource.size))
        {
            if (resource.idx == -1)
                resource.idx = i;

            record->size = xget_file_size(fname, fd) - resource.size;
            xcopyfile_range(fname, fd, out, outfd, 0, record->size);
        } else {
            record->size = xcopyfile(fname, fd, out, outfd);
        }

        offset = binary_offset + record->size;

        // done with this binary!
        xclose(fname, fd);
    } // for

    // Write the actual FatELF header now...
    xwrite_fatelf_header(out, outfd, header);

    // rather then perform any complex merging of resources, we select the
    // resources from the first file.
    if (resource.idx >= 0) {
        const char *fname = bins[resource.idx];
        const int fd = xopen(fname, O_RDONLY, 0755);

        if (haiku_rsrc_offset(out, outfd, &offset)) {
            xlseek(out, outfd, offset, SEEK_SET);
            xcopyfile_range(fname, fd, out, outfd, resource.offset,
                resource.size);
        }

        xclose(fname, fd);
    }

    xclose(out, outfd);
    free(header);

    unlink_on_xfail = NULL;

    return 0;  // success.
} // fatelf_glue

static int fatelf_file_merge(FTSENT *ent, const char *out) {
    switch (ent->fts_info) {
        case FTS_DEFAULT:
            fprintf(stderr, "OTHER: %s -> %s\n", ent->fts_accpath, out);
            break;

        case FTS_F:
            fprintf(stderr, "FILE: %s -> %s\n", ent->fts_accpath, out);
            break;

        case FTS_D:
            if (mkdir(out, 0700) == -1 && errno != EEXIST)
                xfail("Failed to create directory '%s': %s", out, strerror(errno));
            break;

        case FTS_SL:
        case FTS_SLNONE: {
            fprintf(stderr, "LINK: %s -> %s\n", ent->fts_accpath, out);

            // The link's target byte length is available via st_size
            off_t linksize = ent->fts_statp->st_size;
            char *linkname = xmalloc(linksize + 1);

            // Read the link target
            ssize_t len = readlink(ent->fts_accpath, linkname, linksize);
            if (len == -1) {
                fprintf(stderr, "Failed to read symlink '%s': %s",
                        ent->fts_accpath, strerror(errno));
                free(linkname);
                return 1;
            }
            if (len > linksize)
                xfail("Symlink '%s' increased in size "
                      "between lstat() and realink()", ent->fts_accpath);

            // NULL terminate
            linkname[linksize] = '\0';

            // Create the link
            if (symlink(linkname, out) == -1 && errno != EEXIST) {
                xfail("Failed to create symlink '%s': %s", out,
                      strerror(errno));
                free(linkname);
                return 1;
            }

            free(linkname);
            break;
        }

        default:
            fprintf(stderr, "Skipping unknown file type '%s'", ent->fts_accpath);
            return 1;
    }

    xcopyfile_attr(ent->fts_accpath, out);
    return 0;
}

static int fatelf_recursive_glue(const char *outdir, const char **dirs,
    const int dircount)
{
    FTS *tree;
    FTSENT *ent;
    size_t outlen;
    int i;

    outlen = strlen(outdir);

    // fts(3) requires a NULL-terminated array
    for (i = 0; i < dircount; i++) {
        const char *dir = dirs[i];
        const char *path_argv[] = { dir, NULL };

        tree = xfts_open((char * const *) path_argv, FTS_NOCHDIR|FTS_PHYSICAL, NULL);
        while ((ent = xfts_read(tree)) != NULL) {
            char *target;
            {
                size_t target_len;
                const char *relpath;

                relpath = ent->fts_path + strlen(dir);
                target_len = outlen + strlen(relpath) + 1;
                target = xmalloc(target_len);

                strncpy(target, outdir, target_len);
                strncat(target, relpath, target_len-outlen);
            }

            // Skip post-order visited directories
            if (ent->fts_info == FTS_DP)
                continue;

            fatelf_file_merge(ent, target);
        }

        xfts_close(tree);
    }

    return 0;  // success
} // fatelf_recursive_glue


static void xusage (const char *argv0) {
    xfail("USAGE:\n"
          "  %s <out> <bin1> <bin2> [... binN]\n"
          "  %s -r <out> <dir1> <dir2> [... dirN]", argv0, argv0);
} // xusage

int main(int argc, const char **argv)
{
    const char *argv0 = argv[0];
    int rflag = 0;
    int ch;

    xfatelf_init(argc, argv);
    while ((ch = getopt(argc, (char **) argv, "r"))  != -1) {
       switch (ch) {
           case 'r':
               rflag = 1;
               break;

           default:
               xusage(argv0);
               break;
       }
    }
    argc -= optind;
    argv += optind;

    if (argc < 2)
        xusage(argv0);

    if (rflag) {
        return fatelf_recursive_glue(argv[0], &argv[1], argc - 1);
    } else {
        return fatelf_glue(argv[0], &argv[1], argc - 1);
    }
} // main

// end of fatelf-glue.c ...

