/*
 * Copyright 2012, Landon Fuller <landonf@bikemonkey.org>.
 * All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Implements a front-end arch-aware driver for GNU as(1).
 * Based on the design of Apple's as/driver.c in cctools-836.
 */

#define FATELF_UTILS 1
#include "fatelf-utils.h"
#include <stdbool.h>

#if TODO
static const char *exec_paths[] = {
		"../libexec/as/",
		"../local/libexec/as/",
		NULL
};
#endif

typedef struct as_flag {
	const char opt;
	const char *long_opt;
	const bool accepts_arg;
	const bool fat_arg;
	const bool single_dash;
} as_flag;

static const as_flag as_flags[] = {
		/* opt	long_opt	accept_arg	fat_arg		single_dash (long opt) */
		{ 'o',	NULL,		true,		false,		false	},
		{ '\0', "defsyms",	true,		false,		false	},
		{ '\0',	"arch",		true,		true,		true	}
};

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

int main(int argc, const char **argv)
{
	const char *as_argv[argc];
	int as_argc = argc;
	int as_argi = 1;
	int i;

	/* Not that this should be possible */
	if (argc < 1)
		return 1;

	/* Determine the install prefix of our binary */
	char *prefix = realpath(argv[0], NULL);
	{
		if (prefix == NULL)
			xfail("Could not resolve absolute path to %s", argv[0]);

		char *prefix_tail = rindex(prefix, '/');
		if (prefix_tail == NULL)
			xfail("Could not find enclosing directory of path %s", prefix);
		*prefix_tail = '\0';
	}

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

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
			printf("TODO: Unhandled @FILE argument: '%s'\n", arg);
		}

		if (flag != NULL && flag->fat_arg) {
			// TODO - handle fat flags
			printf("Got fat flag: %s\n", arg);
			as_argc--;

			if (flag->accepts_arg) {
				i++;
				if (i < argc)
					printf("Got fat arg: %s\n", argv[i]);

				as_argc--;
			}
		} else {
			as_argv[as_argi] = arg;
			as_argi++;

			/* If the argument accepts a flag, drop the flag into place */
			if (flag != NULL && flag->accepts_arg) {
				i++;
				if (i < argc) {
					as_argv[as_argi] = argv[i];
					as_argi++;
				}
			}
		}
	}

	// TODO!
	as_argv[0] = "as-todo";

	printf("as arguments: ");
	for (i = 0; i < as_argc; i++) {
		printf("%s ", as_argv[i]);
	}
	printf("\n");

	free(prefix);
	return 0;
} // main

// end of fatelf-as.c ...
