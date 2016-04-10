#include "flags.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

Flags global_flags;

void usage()
{
	fprintf(stderr, "Usage: nageru [OPTION]...\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "  -h, --help                      print usage information\n");
	fprintf(stderr, "  -c, --num-cards                 set number of input cards (default 2)\n");
	fprintf(stderr, "  -t, --theme=FILE                choose theme (default theme.lua)\n");
	fprintf(stderr, "  -v, --va-display=SPEC           VA-API device for H.264 encoding\n");
	fprintf(stderr, "                                    ($DISPLAY spec or /dev/dri/render* path)\n");
	fprintf(stderr, "      --http-uncompressed-video   send uncompressed NV12 video to HTTP clients\n");
	fprintf(stderr, "      --flat-audio                start with most audio processing turned off\n");
}

void parse_flags(int argc, char * const argv[])
{
	static const option long_options[] = {
		{ "help", no_argument, 0, 'h' },
		{ "num-cards", required_argument, 0, 'c' },
		{ "theme", required_argument, 0, 't' },
		{ "va-display", required_argument, 0, 1000 },
		{ "http-uncompressed-video", no_argument, 0, 1001 },
		{ "flat-audio", no_argument, 0, 1002 },
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
		case 1002:
			global_flags.flat_audio = true;
			break;
		case 'h':
			usage();
			exit(0);
		default:
			fprintf(stderr, "Unknown option '%s'\n", argv[option_index]);
			fprintf(stderr, "\n");
			usage();
			exit(1);
		}
	}
}
