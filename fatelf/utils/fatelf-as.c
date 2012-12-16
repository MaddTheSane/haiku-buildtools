/*
 * Copyright 2012, Landon Fuller <landonf@bikemonkey.org>.
 * All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Implements a front-end arch-aware driver for GNU as(1).
 * Models the functionality of Apple's as/driver.c in cctools-836.
 */

#define FATELF_UTILS 1
#include "fatelf-utils.h"

#include <errno.h>
#include <stdbool.h>
#include <unistd.h>

static const char *exec_paths[] = {
    "/../libexec/as/",
    "/../local/libexec/as/",
};

static const char *bin_path = "/bin/as";

#define MAX_FILE_DEPTH 100

typedef struct as_flag {
    const char opt;
    const char *long_opt;
    const bool accepts_arg;
    const bool fat_arg;
    const bool single_dash;
} as_flag;

static const as_flag as_flags[] = {
    /* opt    long_opt    accept_arg    fat_arg        single_dash (long opt) */
    { 'o',    NULL,       true,         false,         false    },
    { 'I',    NULL,       true,         false,         false    },
    { '\0',   "defsyms",  true,         false,         false    },
    { '\0',   "arch",     true,         true,          true    },

    // MIPS-specific arguments
    { 'G',    NULL,        true,        false,        false    },
};

typedef struct arch_as_entry {
    const char *arch[10];
    const char *as_arch[10];
} arch_as_entry;

// Map -arch flags to as(1) architecture names.
static const arch_as_entry arch_as_map[] = {
    {
        { "i686", "i586", "i486", "i386", NULL },
        { "x86", "i386", NULL }
    },
    {
        { "x86_64", "x86-64", NULL },
        { "x86_64", "x86-64", NULL }
    },
    {
        { "arm", "armv4t", "xscale", "armv5", "armv6", "armv7", NULL },
        { "arm", NULL }
    },
    {
        { "ppc", NULL },
        { "powerpc", "ppc", NULL }
    },
    {
        { "ppc64", NULL },
        { "powerpc64", "ppc64", NULL }
    },
    {
        { "m68k", NULL },
        { "m68k", NULL }
    },
};

typedef struct arg_table {
    int argc;
    char **argv;
    size_t argv_length;
} arg_table;

static void append_argument (arg_table *args, const char *argument);
static void parse_arguments (arg_table *input_args, arg_table *output_args,
        arg_table *fat_args, int depth);
static void parse_argument_file (const char *fname, arg_table *output_args,
        arg_table *fat_args, int depth);

/* Look up the given option in the as_flags table. */
static const as_flag *find_flag (const char opt,
        const char *long_opt,
        bool single_dash)
{
    int i;

    for (i = 0; i < sizeof(as_flags) / sizeof(struct as_flag); i++) {
        const as_flag *flag = &as_flags[i];
        if (opt != '\0' && flag->opt == opt) {
            return flag;
        } else if (long_opt != NULL && flag->long_opt != NULL) {
            if (strcmp(long_opt, flag->long_opt) == 0 &&
                    single_dash == flag->single_dash)
            {
                return flag;
            }
        }
    }

    return NULL;
}

/* Find an arch->as map entry for the given fat arch. If none is found, NULL
 * is returned. */
static const arch_as_entry *arch_as_lookup (const char *fat_arch) {
    int i;
    for (i = 0; i < sizeof(arch_as_map) / sizeof(arch_as_map[0]); i++) {
        const arch_as_entry *entry;
        int arch_idx;

        /* See if fat_arch matches this entry */
        entry = &arch_as_map[i];
        for (arch_idx = 0; entry->arch[arch_idx] != NULL; arch_idx++) {
            if (strcmp(fat_arch, entry->arch[arch_idx]) == 0)
                return entry;
        }
    }

    return NULL;
}

/* Append an argument to the provided args table. The caller is responsible
 * for free()'ing the argument added to the table. */
static void append_argument (arg_table *args, const char *argument) {
    /* Ensure adequate space in the buffer. We grow in blocks of
     * 10 -- this number was arbitrarily chosen. */
    if (args->argv == NULL) {
        args->argv_length = 10;
        args->argv = xmalloc(sizeof(char *) * args->argv_length);
    } else if (args->argc+1 >= args->argv_length) {
        args->argv_length += 10;
        args->argv = xrealloc(args->argv, sizeof(char *) * args->argv_length);
    }

    args->argv[args->argc] = xstrdup(argument);
    args->argc++;
    args->argv[args->argc] = NULL;
}

/* Parse an as(1) @FILE, which contains command line arguments, seperated
 * by whitespace. */
static void parse_argument_file (const char *fname,
        arg_table *output_args,
        arg_table *fat_args,
        int depth)
{
    arg_table file_args;
    char optbuf[8192];
    int scancount;
    FILE *file;
    int fd;
    int i;

    /* Protect against infinite recursion */
    if (depth >= MAX_FILE_DEPTH)
        xfail("Exceeded maximum number of supported @FILE includes in '%s'",
            fname);

    /* Open the input file */
    fd = xopen(fname, O_RDONLY, 0);
    file = fdopen(fd, "r");

    /* Configure table to hold parsed arguments */
    file_args.argc = 0;
    file_args.argv = NULL;
    file_args.argv_length = 0;

    do {
        optbuf[sizeof(optbuf)-1] = '\0';
        scancount = fscanf(file, "%8192s", optbuf);
        if (scancount == EOF)
            break;

        if (scancount == 0)
            xfail("Failed to parse input file: %s", fname);

        if (optbuf[sizeof(optbuf)-1] != '\0')
            xfail("Unable to handle options larger than 8192");

        append_argument(&file_args, optbuf);
    } while (1);

    /* Recursively parse the input arguments */
    parse_arguments(&file_args, output_args, fat_args, depth);

    /* Clean up */
    xclose(fname, fd);
    for (i = 0; i < file_args.argc; i++)
        free(file_args.argv[i]);
}

/* Parse all arguments from input_args, appending as(1) arguments to
 * output_args, and FAT-specific arguments to fat_args. */
static void parse_arguments (arg_table *input_args,
        arg_table *output_args,
        arg_table *fat_args,
        int depth)
{
    int i;
    for (i = 0; i < input_args->argc; i++) {
        const char *arg = input_args->argv[i];

        const as_flag *flag = NULL;

        /*
         * Determine whether the argument(s) accept an argument, or are
         * fat-specific arguments that should not be passed to as(1). We
         * skip --, which is used to inform as(1) that it should read from
         * stdin.
         *
         * as(1) additionally allows for single-letter flags to be grouped
         * such that -abc is the same as -a -b -c, so we must extract and
         * parse those individually.
         */
        if (arg[0] == '-' && arg[1] != '-') {
            const char *opt = arg+1;

            /* Special case any single-dash "long" opts, eg, -arch. */
            flag = find_flag('\0', opt, true);
            if (flag == NULL) {
                /* Handle grouped single-char flags. We only need to interpret
                 * the first argument that matches a known flag; we can leave
                 * detection of missing arguments, etc, to as(1). */
                for (opt = arg+1; *opt != '\0'; opt++) {
                    flag = find_flag(*opt, NULL, true);
                    if (flag != NULL)
                        break;
                }
            }
        } else if (arg[0] == '-' && arg[1] == '-' && arg[2] != '\0') {
            const char *opt = arg+2;
            flag = find_flag('\0', opt, false);
        } else if (arg[0] == '@' && arg[1] != '\0') {
            /* Recursively parse the argument @FILE */
            parse_argument_file(arg+1, output_args, fat_args, depth + 1);
        }

        if (flag != NULL && flag->fat_arg) {
            append_argument(fat_args, arg);
            if (flag->accepts_arg) {
                i++;
                if (i < input_args->argc)
                    append_argument(fat_args, input_args->argv[i]);
            }
        } else {
            append_argument(output_args, arg);

            /* If the argument accepts a flag, drop the flag into place */
            if (flag != NULL && flag->accepts_arg) {
                i++;
                if (i < input_args->argc)
                    append_argument(output_args, input_args->argv[i]);
            }
        }
    }
}

/* Allocate and initialize a path to an as(1) binary. The caller is responsible
 * for free()'ing the result. */
static char *make_assembler_path (const char *prefix, const char *exec_path,
        const char *fat_arch)
{
    char *path;
    size_t len;

    len = strlen(prefix) + strlen(exec_path) + strlen(fat_arch) +
            strlen(bin_path) + 1;
    path = xmalloc(len);

    strcpy(path, prefix);
    strcat(path, exec_path);
    strcat(path, fat_arch);
    strcat(path, bin_path);

    return path;
}

int main(int argc, const char **argv)
{
    const char *fat_arch;
    arg_table input_args;
    arg_table as_args;
    arg_table fat_args;
    int i;

    /* Not that this should be possible */
    if (argc < 1)
        return 1;

    /* Determine the install prefix of our binary */
    char *prefix = xgetexecname(argv[0]);
    {
        char *prefix_tail = rindex(prefix, '/');
        if (prefix_tail == NULL)
            xfail("Could not find enclosing directory of path %s", prefix);
        *prefix_tail = '\0';
    }

    /* Configure our input/output argument tables */
    as_args.argc = 0;
    as_args.argv_length = 0;
    as_args.argv = NULL;
    append_argument(&as_args, "as");

    fat_args.argc = 0;
    fat_args.argv_length = 0;
    fat_args.argv = NULL;

    input_args.argc = argc - 1;
    input_args.argv_length = input_args.argc;
    input_args.argv = (char **) argv + 1;

    /* Parse all input arguments */
    parse_arguments(&input_args, &as_args, &fat_args, 0);

    /* Parse our fat arguments */
    fat_arch = NULL;
    for (i = 0; i < fat_args.argc; i++) {
        const char *arg = fat_args.argv[i];
        if (strcmp(arg, "-arch") == 0) {
            if (fat_arch != NULL)
                xfail("more than one -arch option (not allowed, use cc(1) instead)");

            i++;
            if (i >= fat_args.argc)
                xfail("missing argument to -arch option");

            fat_arch = fat_args.argv[i];
        } else {
            /* Should never happen; these arguments have already
             * been validated against the list of defined fat arguments. */
            xfail("Unknown argument %s", arg);
        }
    }

    /* Configure the fat architecture */
    if (fat_arch == NULL) {
        /* Determine the host architecture */
        const fatelf_machine_info *machine = get_machine_from_host();
        if (machine == NULL)
            xfail("Can not determine host machine type");
        fat_arch = machine->name;
    } else {
        /* Try to map to a known architecture */
        const fatelf_machine_info *machine = get_machine_by_name(fat_arch);
        if (machine != NULL)
            fat_arch = machine->name;
    }

    /* Find the path to the as(1) binary */
    arg_table paths;
    memset(&paths, 0, sizeof(paths));

    for (i = 0; i < sizeof(exec_paths) / sizeof(exec_paths[0]); i++) {
        const arch_as_entry *as_arch_map = arch_as_lookup(fat_arch);
        const char *exec_path = exec_paths[i];
        char *path;
        int j;

        /* Attempt to execute an assembler using the mapped as(1) binary names,
         * or the default fat_arch, if no mapping is found. */
        if (as_arch_map != NULL) {
            for (j = 0; as_arch_map->as_arch[j] != NULL; j++) {
                const char *as_name = as_arch_map->as_arch[j];
                path = make_assembler_path(prefix, exec_path, as_name);
                append_argument(&paths, path);
                free(path);
            }
        } else {
            path = make_assembler_path(prefix, exec_path, fat_arch);
            append_argument(&paths, path);
            free(path);
        }

        /* Try to execute the assembler */
        for (j = 0; j < paths.argc; j++) {
            const char *path = paths.argv[j];
            if (access(path, X_OK) == 0) {
                free(as_args.argv[0]);
                as_args.argv[0] = strdup(path);

                execv(path, as_args.argv);
                xfail("Could not execute as(1): %s", strerror(errno));
            }
        }
    }

    /* Report the failure, and provide a list of installed assemblers */
    fprintf(stderr, "Assembler for arch %s not found. Attempted paths:\n",
            fat_arch);
    for (i = 0; i < paths.argc; i++) {
        fprintf(stderr, "  %s\n", paths.argv[i]);
    }

    /* Clean up */
    free(prefix);
    for (i = 0; i < as_args.argc; i++)
        free(as_args.argv[i]);

    for (i = 0; i < fat_args.argc; i++)
        free(fat_args.argv[i]);

    return 1;
} // main

// end of fatelf-as.c ...
