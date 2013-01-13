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

static void xverify_file_matches (const char *f1, const char *f2) {
    struct stat st1, st2;
    xlstat(f1, &st1);
    xlstat(f2, &st2);

    // Check the file type
    if ((st1.st_mode & S_IFMT) != (st2.st_mode & S_IFMT))
        xfail("File '%s' is of a different type than '%s'", f1, f2);
}

static int fatelf_merge_files(const char *out, const char **files,
    const int filecount)
{
    struct stat st;

    if (filecount == 0)
        return 0;

    const char *in = files[0];

    xlstat(in, &st);
    switch (st.st_mode & S_IFMT) {
        case S_IFDIR:
            if (mkdir(out, 0700) == -1) {
                if (errno == EEXIST)
                    xverify_file_matches(in, out);
                else
                    xfail("Failed to create directory '%s': %s", out,
                        strerror(errno));
            }
            break;

        case S_IFREG: {
            int i;

            // Determine if this is an ELF file
            int elfd = xopen(in, O_RDONLY, 0600);
            uint8_t magic[4];
            xread(in, elfd, &magic, sizeof(magic), 1);
            if (memcmp(magic, (uint8_t[]){ 0x7F, 'E', 'L', 'F' },
                sizeof(magic)) == 0 && filecount > 1)
            {
                fatelf_glue(out, files, filecount);
            } else if (0 /* ar archive header */) {
            } else {
                size_t bufsize = 4096;
                int fds[filecount];
                ssize_t nread[filecount];
                uint8_t **buffers = xmalloc(sizeof(void *) * filecount);
                off_t nleft = st.st_size;
                int outfd = xopen(out, O_WRONLY|O_CREAT|O_TRUNC, 0600);

                for (i = 0; i < filecount; i++) {
                    buffers[i] = xmalloc(bufsize);
                    fds[i] = xopen(files[i], O_RDONLY, 0600);
                }

                // read in data from all input files, check for equality, and
                // then write to the output
                while (nleft > 0) {
                    for (i = 0; i < filecount; i++) {
                        if (fds[i] == -1)
                            continue;

                        nread[i] = xread(files[i], fds[i], buffers[i], bufsize, 0);

                        if (i > 0) {
                            /* Check for file equality */
                            if (nread[i] != nread[0]) {
                                fprintf(stderr, "Files '%s' and '%s' differ in length\n", files[i], files[0]);
                                xclose(files[i], fds[i]);
                                fds[i] = -1;
                                continue;
                            }

                            if (memcmp(buffers[0], buffers[i], nread[0]) != 0) {
                                fprintf(stderr, "Files '%s' and '%s' differ\n", files[i], files[0]);
                                xclose(files[i], fds[i]);
                                fds[i] = -1;
                                continue;
                            }

                        } else if (i == 0) {
                            /* Write to output */
                            xwrite(out, outfd, buffers[i], nread[i]);
                        }
                    }

                    nleft -= nread[0];
                }

                // Clean up
                for (i = 0; i < filecount; i++) {
                    free(buffers[i]);
                    if (fds[i] != -1)
                        xclose(files[i], fds[i]);
                }

                free(buffers);
                xclose(out, outfd);
            }
            break;
        }

        case S_IFLNK: {
            // The link's target byte length is available via st_size
            off_t linksize = st.st_size;
            char *linkname = xmalloc(linksize + 1);

            // Read the link target
            ssize_t len = readlink(in, linkname, linksize);
            if (len == -1) {
                fprintf(stderr, "Failed to read symlink '%s': %s",
                        in, strerror(errno));
                free(linkname);
                return 1;
            }
            if (len > linksize)
                xfail("Symlink '%s' increased in size "
                      "between lstat() and realink()", in);

            // NULL terminate
            linkname[linksize] = '\0';

            // Create the link
            if (symlink(linkname, out) == -1) {
                if (errno == EEXIST) {
                    xverify_file_matches(in, out);
                } else {
                    xfail("Failed to create symlink '%s': %s", out,
                          strerror(errno));
                }
            }

            free(linkname);
            break;
        }
        default:
            xfail("Unsupported input file type of %s",
                file_type_name(st.st_mode));
            break;
    }

    // TODO : Verify this only once? Check the differences?
    xcopyfile_attr(in, out);

    return 0;
}

static int fatelf_recursive_glue(const char *outdir, const char **dirs,
    const int dircount)
{
    FTS *tree;
    FTSENT *ent;
    size_t outlen;
    int ftsidx, diridx;

    outlen = strlen(outdir);

    // Verify that all the input paths are directories
    for (diridx = 0; diridx < dircount; diridx++) {
        struct stat st;

        xlstat(dirs[diridx], &st);
        if (!S_ISDIR(st.st_mode))
            xfail("Input path '%s' is not a directory.\n", dirs[diridx]);
    }

    // Iterate over the input directories. For each file found, we immediately
    // search all other matching directories for a corresponding path, and then
    // perform the merge on all matching files.
    //
    // If the destination file already  exists, this is not the first iterated
    // directory, AND the path exists in a previously iterated directory, then
    // we can assume a merge already took place and skip the file.
    for (ftsidx = 0; ftsidx < dircount; ftsidx++) {
        const char *dir = dirs[ftsidx];
        const char *path_argv[] = { dir, NULL };

        tree = xfts_open((char * const *) path_argv, FTS_NOCHDIR|FTS_PHYSICAL, NULL);
        while ((ent = xfts_read(tree)) != NULL) {
            char *files[dircount];
            int filecount;
            const char *relpath;
            size_t relpath_len;
            char *target;
            struct stat st;
            int j;

            // Skip post-order visited directories
            if (ent->fts_info == FTS_DP)
                continue;

            // Compute the relative path of the file, along with absolute path
            // to the target directory. The relative path will be used to find
            // matching files to merge from the other input directories.
            {
                size_t target_len;
                relpath = ent->fts_path + strlen(dir);
                relpath_len = strlen(relpath);
                target_len = outlen + strlen(relpath) + 1;
                target = xmalloc(target_len);

                strncpy(target, outdir, target_len);
                strncat(target, relpath, target_len-outlen);
            }

            // Build up the list of matching input files from all input
            // directories
            filecount = 0;
            bool merge_done = false;
            for (diridx = 0; diridx < dircount; diridx++) {
                // Generate the absolute path for the file
                const char *dir = dirs[diridx];
                size_t inpath_len = strlen(dir) + relpath_len + 1;
                char *inpath = xmalloc(inpath_len);

                strcpy(inpath, dir);
                strcat(inpath, relpath);

                // We can avoid duplicate merges if we know that the merge
                // already occurred during an earlier FTS iteration.
                if (ftsidx > 0 && lstat(target, &st) == 0) {
                    for (j = 0; j < filecount; j++) {
                        if (lstat(files[j], &st) == 0)
                            merge_done = true;
                    }
                }

                // If the input path exists, add it to the input list and
                // verify that it matches the file type of the other files
                // already in the list
                if (lstat(inpath, &st) == 0) {
                    files[filecount] = inpath;
                    filecount++;

                    if (filecount > 0)
                        xverify_file_matches(inpath, files[0]);
                }
            }

            assert(filecount > 0);

            if (!merge_done)
                fatelf_merge_files(target, (const char **) files, filecount);

            for (j = 0; j < filecount; j++)
                free(files[j]);

            free(target);
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

