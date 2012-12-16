/*
 * Copyright 2012, Landon Fuller <landonf@bikemonkey.org>.
 * All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Implements a front-end arch-aware driver for gcc(1).
 * Models the functionality of Apple's 'driverdriver.c' in gcc-5666.3
 */

#define FATELF_UTILS 1
#include "fatelf-utils.h"

#include <stdbool.h>

static const char xarch_flag[] = "-Xarch_";

#define MAX_FILE_DEPTH 10

typedef struct arg_table {
    int argc;
    char **argv;
    size_t argv_length;
} arg_table;

typedef struct compiler {
    char *fat_arch;
    arg_table args;
} compiler;

typedef struct compiler_set {
    arg_table default_args;
    compiler **compilers;
    size_t count;
} compiler_set;

typedef struct cc_flag {
    const char *opt;
    const bool accepts_arg;
    const bool driver_flag;
    const bool driver_only;
    const bool fat_nocompat;
} cc_flag;

static const cc_flag cc_flags[] = {
    /*  opt         accepts_arg driver_flag driver_only fat_nocompat */
    {   "-o",       true,       true,       true,       false   },
    {   "-c",       false,      true,       false,      false   },
    {   "-S",       false,      false,      false,      true    },
    {   "-E",       false,      false,      false,      true    },
    {   "-MD",      false,      false,      false,      true    },
    {   "-MMD",     false,      false,      false,      true    },
    {   "-m32",     false,      true,       false,      false   },
    {   "-m64",     false,      true,       false,      false   },
    // TODO - Add compiler opts that accept args
    {   "-arch",    true,       true,       true,       false   },
};

static void parse_arguments (arg_table *input_args, arg_table *driver_args,
        arg_table *nofat_args, compiler_set *compilers, int depth);

/* Look up the given option in the cc_flags table. */
static const cc_flag *find_flag (const char *opt)
{
    int i;

    for (i = 0; i < sizeof(cc_flags) / sizeof(cc_flags[0]); i++) {
        const cc_flag *flag = &cc_flags[i];
        if (strcmp(opt, flag->opt) == 0)
            return flag;
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

/* Find a compiler matching the given arch, or return NULL */
static compiler *find_compiler (compiler_set *compilers, const char *arch) {
    int i;

    for (i = 0; i < compilers->count; i++) {
        compiler *c = compilers->compilers[i];
        if (strcmp(c->fat_arch, arch) == 0)
            return c;
    }

    return NULL;
}

/* Append and return a new compiler to the compiler set for the given arch. If
 * the architecture is already available from the compiler set, the existing
 * compiler is returned. */
static compiler *append_compiler (compiler_set *compilers, const char *arch) {
    compiler *c;
    int i;

    // Prefer an existing compiler
    c = find_compiler(compilers, arch);
    if (c != NULL)
        return c;

    // Initialize a new compiler
    c = xmalloc(sizeof(*c));
    memset(c, 0, sizeof(*c));

    c->fat_arch = strdup(arch);
    append_argument(&c->args, "gcc");

    // Copy in all previously parsed arguments
    for (i = 0; i < compilers->default_args.argc; i++)
        append_argument(&c->args, compilers->default_args.argv[i]);

    // Append to the compiler set
    compilers->count++;
    compilers->compilers = xrealloc(compilers->compilers, compilers->count * sizeof(*c));
    compilers->compilers[compilers->count - 1] = c;

    return c;
}

/* Append a compiler argument to the compiler set. If arch_only is non-NULL,
 * the argument will only be applied to a matching compiler. New compilers
 * will be automatically initialized. */
static void append_compiler_argument (compiler_set *compilers,
        const char *argument,
        const char *arch_only)
{
    int i;

    // Handle non-architecture-specific arguments
    if (arch_only == NULL) {
        append_argument(&compilers->default_args, argument);
        for (i = 0; i < compilers->count; i++) {
            append_argument(&compilers->compilers[i]->args, argument);
        }

        return;
    }

    // Fetch (or create) the compiler entry
    compiler *c = NULL;
    for (i = 0; i < compilers->count; i++) {
        if (strcmp(compilers->compilers[i]->fat_arch, arch_only) == 0) {
            c = compilers->compilers[i];
            break;
        }
    }

    if (c == NULL)
        c = append_compiler(compilers, arch_only);

    // Apply the compiler-specific argument
    append_argument(&c->args, argument);
}

/* Parse an gcc(1) @file, which contains command line arguments, separated
 * by whitespace. */
static void parse_argument_file (const char *fname, arg_table *driver_args,
        arg_table *nofat_args, compiler_set *compilers, int depth)
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
    memset(&file_args, 0, sizeof(file_args));

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
    parse_arguments(&file_args, driver_args, nofat_args, compilers, depth);

    /* Clean up */
    xclose(fname, fd);
    for (i = 0; i < file_args.argc; i++)
        free(file_args.argv[i]);
}

/* Parse all arguments from input_args, populating compilers and fat_args. */
static void parse_arguments (arg_table *input_args, arg_table *driver_args,
        arg_table *nofat_args, compiler_set *compilers, int depth)
{
    int i;

    for (i = 0; i < input_args->argc; i++) {
        const char *arg = input_args->argv[i];

        // Handle -Xarch_*
        const char *arch_only = NULL;
        if (strncmp(arg, xarch_flag, strlen(xarch_flag)) == 0) {
            // Extract the target architecture
            const char *p = arg + strlen(xarch_flag);
            if (*p != '\0') {
                arch_only = p;

                // Advance to the actual flag
                i++;
                if (i == input_args->argc)
                    break;
                arg = input_args->argv[i];
            }
        }

        // Handle @file
        if (arg[0] == '@' && arg[1] != '\0') {
            parse_argument_file(arg+1, driver_args, nofat_args, compilers,
                    depth + 1);
            continue;
        }

        const cc_flag *flag = find_flag(arg);
        if (flag == NULL) {
            append_compiler_argument(compilers, arg, arch_only);
        } else {
            if (flag->driver_flag)
                append_argument(driver_args, arg);

            if (!flag->driver_only)
                append_compiler_argument(compilers, arg, arch_only);

            if (flag->fat_nocompat)
                append_argument(nofat_args, arg);

            if (flag->accepts_arg) {
                i++;

                if (i >= input_args->argc)
                    xfail("argument to '%s' is missing (expected 1 value)", arg);

                arg = input_args->argv[i];
                if (flag->driver_flag)
                    append_argument(driver_args, arg);

                if (!flag->driver_only)
                    append_compiler_argument(compilers, arg, arch_only);
            }
        }
    }
}

int main(int argc, const char **argv)
{
    arg_table input_args;
    arg_table driver_args;
    arg_table nofat_args;

    compiler_set compilers;
    int i;

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

    /* Initialize our input/output argument tables */
    memset(&compilers, 0, sizeof(compilers));
    memset(&driver_args, 0, sizeof(driver_args));
    memset(&nofat_args, 0, sizeof(nofat_args));

    input_args.argc = argc - 1;
    input_args.argv_length = input_args.argc;
    input_args.argv = (char **) argv + 1;

    /* Parse all input arguments */
    parse_arguments(&input_args, &driver_args, &nofat_args, &compilers, 0);

    /* Handle any driver-specific arguments. Note that the existence
     * of required flags has already been verified. */
    for (i = 0; i < driver_args.argc; i++) {
        const char *arg = driver_args.argv[i];

        if (strcmp(arg, "-arch") == 0) {
            i++;
            append_compiler(&compilers, driver_args.argv[i]);
        }

        // TODO - handle additional driver opts
    }


    /* Report any arguments incompatible with multi-arch execution */
    if (compilers.count > 1 && nofat_args.argc > 0) {
        for (i = 0; i < nofat_args.argc; i++) {
            const char *arg = nofat_args.argv[i];
            fprintf(stderr, "%s is not supported with multiple -arch flags\n",
                    arg);
        }
        exit(1);
    }


    /* If no FAT compilers found, add default */
    if (compilers.count == 0) {
        const fatelf_machine_info *machine = get_machine_from_host();
        append_compiler(&compilers, machine->name);
    }

    // TODO: Execute the compilers!
    for (i = 0; i < compilers.count; i++) {
        compiler *c = compilers.compilers[i];
        printf("%s: ", c->fat_arch);

        int argi;
        for (argi = 0; argi < c->args.argc; argi++) {
            printf("%s ", c->args.argv[argi]);
        }

        printf("\n");
    }

    free(prefix);
	return 1;
}
