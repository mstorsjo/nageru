#include "flags.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

Flags global_flags;

void parse_flags(int argc, char * const argv[])
{
	static const option long_options[] = {
		{ "num-cards", required_argument, 0, 'c' },
		{ "theme", required_argument, 0, 't' },
		{ "va-display", required_argument, 0, 1000 },
		{ "http-uncompressed-video", no_argument, 0, 1001 },
		{ 0, 0, 0, 0 }
	};
	for ( ;; ) {
		int option_index = 0;
		int c = getopt_long(argc, argv, "c:t:", long_options, &option_index);

		if (c == -1) {
			break;
		}
		switch (c) {
		case 'c':
			global_flags.num_cards = atoi(optarg);
			break;
		case 't':
			global_flags.theme_filename = optarg;
			break;
		case 1000:
			global_flags.va_display = optarg;
			break;
		case 1001:
			global_flags.uncompressed_video_to_http = true;
			break;
		default:
			fprintf(stderr, "Unknown option '%s'\n", argv[option_index]);
			exit(1);
		}
	}
}
