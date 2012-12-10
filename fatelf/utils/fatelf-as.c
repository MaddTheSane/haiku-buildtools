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

#if TODO
static const char *exec_paths[] = {
		"../libexec/as/",
		"../local/libexec/as/",
		NULL
};
#endif

int main(int argc, const char **argv)
{
	/* Not that this should be possible */
	if (argc < 1)
		return 1;

	return 0;
} // main

// end of fatelf-as.c ...
