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

#include <errno.h>
#include <stdbool.h>
#include <unistd.h>

static const char xarch_flag[] = "-Xarch_";
static const char default_output[] = "a.out";
static const char cmd_prefix[] = "fatelf-";
static const char fatelf_glue_cmd[] = "fatelf-glue";

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

typedef struct arch_cc_entry {
    const char *arch_flag[10];
    const char *cc_arch[10];
} arch_cc_entry;

typedef struct arch_cc_march_entry {
    const char *arch_flag;
    const char *cc_flag[3];
} arch_cc_march_entry;

// This table is used to perform interpretation of the gcc arguments
// before passing through to GCC. The multi-argument option list is
// needed to correctly interpret the GCC flags.
//
// If this list is out-of-sync with GCC, there's a *small* possibility of
// collision between GCC flags and their arguments (eg, '-flag -o' would
// interpret the '-o' as a flag, not an argument). The likelihood of
// this occurring is low, and we have the advantage of being able to
// update GCC and fatelf utils in lockstep.
static const cc_flag cc_flags[] = {
    /*  opt         accepts_arg driver_flag driver_only fat_nocompat */

    // arguments the driver must be aware of
    {   "-o",       true,       true,       true,       false   },
    {   "-c",       false,      false,      false,      false   },

    // driver-specific arguments
    {   "-arch",    true,       true,       true,       false   },

    // fat-incompatible arguments
    {   "-S",       false,      false,      false,      true    },
    {   "-E",       false,      false,      false,      true    },
    {   "-MD",      false,      false,      false,      true    },
    {   "-MMD",     false,      false,      false,      true    },
    {   "-m32",     false,      false,      false,      true    },
    {   "-m64",     false,      false,      false,      true    },

    // multi-argument options
    {   "-D",       true,       false,      false,      false   },
    {   "-U",       true,       false,      false,      false   },
    {   "-e",       true,       false,      false,      false   },
    {   "-T",       true,       false,      false,      false   },
    {   "-u",       true,       false,      false,      false   },
    {   "-I",       true,       false,      false,      false   },
    {   "-m",       true,       false,      false,      false   },
    {   "-x",       true,       false,      false,      false   },
    {   "-L",       true,       false,      false,      false   },
    {   "-A",       true,       false,      false,      false   },
    {   "-V",       true,       false,      false,      false   },

    {   "-Tdata",   true,       false,      false,      false   },
    {   "-Ttext",   true,       false,      false,      false   },
    {   "-Tbss",    true,       false,      false,      false   },
    {   "-include", true,       false,      false,      false   },
    {   "-imacros", true,       false,      false,      false   },
    {   "-aux-info",
                    true,       false,      false,      false   },
    {   "-idirafter",
                    true,       false,      false,      false   },
    {   "-iprefix", true,       false,      false,      false   },
    {   "-iwithprefix",
                    true,       false,      false,      false   },
    {   "-iwithprefixbefore",
                    true,       false,      false,      false   },
    {   "-iwithprefix",
                    true,       false,      false,      false   },
    {   "-iquote",  true,       false,      false,      false   },
    {   "-isystem", true,       false,      false,      false   },
    {   "-isysroot",true,       false,      false,      false   },

};

// Map -arch flags to compiler architecture names.
// FIXME: These should be made non-specific to Haiku.
static const arch_cc_entry arch_cc_map[] = {
    {
        { "x86_64", "i686", "i586", "i486", "i386", NULL },
        { "x86_64-unknown-haiku", "i586-pc-haiku", NULL }
    },
    {
        { "arm", "armv4t", "xscale", "armv5", "armv6", "armv7", NULL },
        { "arm-unknown-haiku", NULL }
    },
    {
        { "ppc", "ppc64", NULL },
        { "powerpc-apple-haiku", NULL }
    },
    {
        { "m68k", NULL },
        { "m68k-unknown-haiku", NULL }
    },
};

// Map -arch flags to compiler -march= flags
static const arch_cc_march_entry arch_cc_march_map[] = {
    { "i386",       {"-m32", NULL }},
    { "i486",       {"-m32", "-march=i486", NULL}},
    { "i586",       {"-m32", "-march=i586", NULL}},
    { "i686",       {"-m32", "-march=i686", NULL}},
    { "x86_64",     {"-m64", NULL }},

    { "arm",        {"-march=armv4t", NULL }},
    { "armv4t",     {"-march=armv4t", NULL }},
    { "armv5",      {"-march=armv5tej", NULL }},
    { "xscale",     {"-march=xscale", NULL }},
    { "armv6",      {"-march=armv6k", NULL}},
    { "armv7",      {"-march=armv7a", NULL }},

    { "ppc601",     {"-m32", "-mcpu=601", NULL}},
    { "ppc603",     {"-m32", "-mcpu=603", NULL}},
    { "ppc604",     {"-m32", "-mcpu=604", NULL}},
    { "ppc604e",    {"-m32", "-mcpu=604e", NULL}},
    { "ppc750",     {"-m32", "-mcpu=750", NULL}},
    { "ppc7400",    {"-m32", "-mcpu=7400", NULL}},
    { "ppc7450",    {"-m32", "-mcpu=7450", NULL}},
    { "ppc970",     {"-m32", "-mcpu=970", NULL}},
    { "ppc64",      {"-m64", NULL}},
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

    // Add any -march flag
    for (i = 0; i < sizeof(arch_cc_march_map) / sizeof(arch_cc_march_map[0]); i++) {
        const arch_cc_march_entry *entry = &arch_cc_march_map[i];
        if (strcmp(arch, entry->arch_flag) == 0) {
            int j;
            for (j = 0; entry->cc_flag[j] != NULL; j++)
                append_argument(&c->args, entry->cc_flag[j]);
            break;
        }
    }

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

/* Find a compiler/tool binary for the given arch. If arch is NULL, a
 * non-arch-specific binary will be found. */
static char *find_tool_bin (const char *prefix, const char *arch,
        const char *cmdname) {
    int i, j, k;

    if (arch == NULL) {
        char *path = xmalloc(strlen(prefix) + strlen(cmdname) + 2);
        strcpy(path, prefix);
        strcat(path, "/");
        strcat(path, cmdname);

        return path;
    }

    for (i = 0; i < sizeof(arch_cc_map)/ sizeof(arch_cc_map[0]); i++) {
        const arch_cc_entry *cc_arch = &arch_cc_map[i];

        for (j = 0; cc_arch->arch_flag[j] != NULL; j++) {
            if (strcmp(cc_arch->arch_flag[j], arch) == 0) {
                for (k = 0; cc_arch->cc_arch[k] != NULL; k++) {
                    const char *cc = cc_arch->cc_arch[k];
                    char *path = xmalloc(strlen(prefix) +
                            strlen(cc) + strlen(cmdname) + 3);
                    strcpy(path, prefix);
                    strcat(path, "/");
                    strcat(path, cc);
                    strcat(path, "-");
                    strcat(path, cmdname);

                    if (access(path, X_OK) == 0)
                        return path;
                }
            }
        }
    }

    return NULL;
}

/* Free all argument elements in the provided argument table */
static void clear_arg_table (arg_table *args) {
    int i;

    for (i = 0; i < args->argc; i++) {
        free(args->argv[i]);
        args->argv[i] = NULL;
    }

    args->argc = 0;
}

/* Unlink all arguments in output_files */
static void clean_output_files (arg_table *output_files) {
    int i;

    for (i = 0; i < output_files->argc; i++)
        unlink(output_files->argv[i]);
}

/* Execute a command */
static bool exec_command (arg_table *args) {
    int ret;
    int stat_loc;
    pid_t pid;

    pid = fork();
    if (pid == 0) {
        execv(args->argv[0], args->argv);
        xfail("exec failed: %s", strerror(errno));
    }

    while ((ret = wait4(pid, &stat_loc, 0, NULL)) == -1) {
        if (errno != EINTR)
            break;
    }

    if (ret == -1) {
        fprintf(stderr, "wait4() failed: %s\n", strerror(errno));
        return false;
    }

    return true;
}

int main(int argc, const char **argv)
{
    arg_table input_args;
    arg_table driver_args;
    arg_table nofat_args;
    arg_table fatelf_glue_args;
    arg_table temp_output_args;
    char *fatelf_glue_path;

    compiler_set compilers;
    int i;

	if (argc < 1)
		return 1;

    /* Determine the install prefix of our binary, along with the target
     * command name (gcc/g++/etc). */
    char *prefix = xgetexecname(argv[0]);
    char *cmdname;
    {
        char *prefix_tail = rindex(prefix, '/');
        if (prefix_tail == NULL)
            xfail("Could not find enclosing directory of path %s", prefix);

        /* Strip the cmd_prefix from the command, in the case where the
         * command is executed as 'fatelf-gcc' or 'fatelf-g++'.  */
        if (strncmp(prefix_tail+1, cmd_prefix, strlen(cmd_prefix)) == 0) {
            cmdname = strdup(prefix_tail+1 + strlen(cmd_prefix));
        } else {
            cmdname = strdup(prefix_tail+1);
        }

        /* NULL terminate the prefix path */
        *prefix_tail = '\0';
    }

    /* Initialize our input/output argument tables */
    memset(&compilers, 0, sizeof(compilers));
    memset(&driver_args, 0, sizeof(driver_args));
    memset(&nofat_args, 0, sizeof(nofat_args));
    memset(&temp_output_args, 0, sizeof(temp_output_args));
    memset(&fatelf_glue_args, 0, sizeof(fatelf_glue_args));

    input_args.argc = argc - 1;
    input_args.argv_length = input_args.argc;
    input_args.argv = (char **) argv + 1;


    /* Find required tools */
    fatelf_glue_path = find_tool_bin(prefix, NULL, fatelf_glue_cmd);
    if (fatelf_glue_path == NULL)
        xfail("Could not find %s", fatelf_glue_cmd);

    /* Parse all input arguments */
    parse_arguments(&input_args, &driver_args, &nofat_args, &compilers, 0);

    /* Handle any driver-specific arguments. Note that the existence
     * of required flags has already been verified. */
    const char *output_file = default_output;
    for (i = 0; i < driver_args.argc; i++) {
        const char *arg = driver_args.argv[i];

        if (strcmp(arg, "-arch") == 0) {
            i++;
            append_compiler(&compilers, driver_args.argv[i]);
        } else if (strcmp(arg, "-o") == 0) {
            i++;
            output_file = driver_args.argv[i];
        }
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

    /* Generate the output file template */
    char *output_template;
    {
        static const char temp_suffix[] = ".XXXXXX";
        char *output_dir = strdup(output_file);
        char *fname = NULL;

        /* Find the directory containing the file, and the file name. */
        char *tail = rindex(output_dir, '/');
        if (tail != NULL) {
            tail++;
            fname = strdup(tail);
            *tail = '\0';
        }

        if (fname != NULL) {
            output_template = xmalloc(strlen(output_dir) +
                    strlen(fname) + strlen(temp_suffix) + 2);
            strcpy(output_template, output_dir);
            strcat(output_template, ".");
            strcat(output_template, fname);
            strcat(output_template, temp_suffix);
        } else {
            output_template = xmalloc(strlen(output_file) +
                    strlen(temp_suffix) + 2);
            strcpy(output_template, ".");
            strcat(output_template, output_file);
            strcat(output_template, temp_suffix);
        }

        free(fname);
        free(output_dir);
    }

    /* Perform initial compilation */
    for (i = 0; i < compilers.count; i++) {
        compiler *c = compilers.compilers[i];
        char *temp_out = NULL;

        /* Configure compiler output path */
        append_argument(&c->args, "-o");
        if (compilers.count == 1) {
            /* If building non-FAT, write to the output file directly */
            append_argument(&c->args, output_file);
        } else {
            temp_out = strdup(output_template);
            if (mkstemp(temp_out) == -1) {
                clean_output_files(&temp_output_args);
                xfail("Could not create temporary output file '%s': %s",
                        temp_out, strerror(errno));
            }

            append_argument(&c->args, temp_out);
            append_argument(&temp_output_args, temp_out);
        }

        /* Find compiler */
        free(c->args.argv[0]);
        c->args.argv[0] = find_tool_bin(prefix, c->fat_arch, cmdname);
        if (c->args.argv[0] == NULL) {
            clean_output_files(&temp_output_args);
            xfail("Could not find compiler for %s in %s",c->fat_arch, prefix);
        }

        if (!exec_command(&c->args)) {
            clean_output_files(&temp_output_args);
            return 1;
        }

        if (temp_out != NULL)
            free(temp_out);
    }

    /* Glue the results */
    if (compilers.count > 1) {
        append_argument(&fatelf_glue_args, fatelf_glue_path);
        append_argument(&fatelf_glue_args, output_file);
        for (i = 0; i < temp_output_args.argc; i++)
            append_argument(&fatelf_glue_args, temp_output_args.argv[i]);

        if (!exec_command(&fatelf_glue_args)) {
            clean_output_files(&temp_output_args);
            return 1;
        }
    }

    /* Clean up */
    clean_output_files(&temp_output_args);

    clear_arg_table(&driver_args);
    clear_arg_table(&nofat_args);
    clear_arg_table(&fatelf_glue_args);
    clear_arg_table(&temp_output_args);

    for (i = 0; i < compilers.count; i++) {
        compiler *c = compilers.compilers[i];
        clear_arg_table(&c->args);
        free(c->fat_arch);
    }
    clear_arg_table(&compilers.default_args);

    free(fatelf_glue_path);
    free(output_template);
    free(prefix);
    free(cmdname);
    return 0;
}
