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

typedef struct arg_table {
    int argc;
    char **argv;
    size_t argv_length;
} arg_table;

typedef struct compiler {
    char *fat_arch;
    arg_table args;
} compiler;

//static void append_argument (arg_table *args, const char *argument);
//static void parse_arguments (arg_table *input_args);

int main(int argc, const char **argv)
{
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

    free(prefix);
	return 1;
}
