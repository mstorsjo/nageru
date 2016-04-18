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
	fprintf(stderr, "      --http-x264-video           send x264-compressed video to HTTP clients\n");
	fprintf(stderr, "      --x264-preset               x264 quality preset (default " X264_DEFAULT_PRESET ")\n");
	fprintf(stderr, "      --x264-tune                 x264 tuning (default " X264_DEFAULT_TUNE ", can be blank)\n");
	fprintf(stderr, "      --http-mux=NAME             mux to use for HTTP streams (default " DEFAULT_STREAM_MUX_NAME ")\n");
	fprintf(stderr, "      --http-audio-codec=NAME     audio codec to use for HTTP streams\n");
	fprintf(stderr, "                                  (default is to use the same as for the recording)\n");
	fprintf(stderr, "      --http-audio-bitrate=KBITS  audio codec bit rate to use for HTTP streams\n");
	fprintf(stderr, "                                  (default is %d, ignored unless --http-audio-codec is set)\n",
		DEFAULT_AUDIO_OUTPUT_BIT_RATE / 1000);
	fprintf(stderr, "      --http-coarse-timebase      use less timebase for HTTP (recommended for muxers\n");
	fprintf(stderr, "                                  that handle large pts poorly, like e.g. MP4)\n");
	fprintf(stderr, "      --flat-audio                start with most audio processing turned off\n");
	fprintf(stderr, "      --no-flush-pbos             do not explicitly signal texture data uploads\n");
	fprintf(stderr, "                                    (will give display corruption, but makes it\n");
	fprintf(stderr, "                                    possible to run with apitrace in real time)\n");
}

void parse_flags(int argc, char * const argv[])
{
	static const option long_options[] = {
		{ "help", no_argument, 0, 'h' },
		{ "num-cards", required_argument, 0, 'c' },
		{ "theme", required_argument, 0, 't' },
		{ "va-display", required_argument, 0, 1000 },
		{ "http-uncompressed-video", no_argument, 0, 1001 },
		{ "http-x264-video", no_argument, 0, 1008 },
		{ "x264-preset", required_argument, 0, 1009 },
		{ "x264-tune", required_argument, 0, 1010 },
		{ "http-mux", required_argument, 0, 1004 },
		{ "http-coarse-timebase", no_argument, 0, 1005 },
		{ "http-audio-codec", required_argument, 0, 1006 },
		{ "http-audio-bitrate", required_argument, 0, 1007 },
		{ "flat-audio", no_argument, 0, 1002 },
		{ "no-flush-pbos", no_argument, 0, 1003 },
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
		case 1004:
			global_flags.stream_mux_name = optarg;
			break;
		case 1005:
			global_flags.stream_coarse_timebase = true;
			break;
		case 1006:
			global_flags.stream_audio_codec_name = optarg;
			break;
		case 1007:
			global_flags.stream_audio_codec_bitrate = atoi(optarg) * 1000;
			break;
		case 1008:
			global_flags.x264_video_to_http = true;
			break;
		case 1009:
			global_flags.x264_preset = optarg;
			break;
		case 1010:
			global_flags.x264_tune = optarg;
			break;
		case 1002:
			global_flags.flat_audio = true;
			break;
		case 1003:
			global_flags.flush_pbos = false;
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

	if (global_flags.uncompressed_video_to_http &&
	    global_flags.x264_video_to_http) {
		fprintf(stderr, "ERROR: --http-uncompressed-video and --http-x264-video are mutually incompatible\n");
		exit(1);
	}
}
