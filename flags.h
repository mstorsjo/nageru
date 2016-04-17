#ifndef _FLAGS_H
#define _FLAGS_H

#include <string>

#include "defs.h"

struct Flags {
	int num_cards = 2;
	std::string va_display;
	bool uncompressed_video_to_http = false;
	std::string theme_filename = "theme.lua";
	bool flat_audio = false;
	bool flush_pbos = true;
	std::string stream_mux_name = DEFAULT_STREAM_MUX_NAME;
};
extern Flags global_flags;

void parse_flags(int argc, char * const argv[]);

#endif  // !defined(_FLAGS_H)
