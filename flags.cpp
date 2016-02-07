#include "flags.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

Flags global_flags;

void parse_flags(int argc, char * const argv[])
{
	static const option long_options[] = {
		{ "num-cards", required_argument, 0, 'c' },
		{ "va-display", required_argument, 0, 1000 },
		{ 0, 0, 0, 0 }
	};
	for ( ;; ) {
		int option_index = 0;
		int c = getopt_long(argc, argv, "c:", long_options, &option_index);

		if (c == -1) {
			break;
		}
		switch (c) {
		case 'c':
			global_flags.num_cards = atoi(optarg);
			break;
		case 1000:
			global_flags.va_display = optarg;
			break;
		default:
			fprintf(stderr, "Unknown option '%s'\n", argv[option_index]);
			exit(1);
		}
	}
}
