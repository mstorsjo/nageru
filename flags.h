#ifndef _FLAGS_H
#define _FLAGS_H

#include <string>

struct Flags {
	int num_cards = 2;
	std::string va_display;
	bool uncompressed_video_to_http = false;
	std::string theme_filename = "theme.lua";
	bool flat_audio = false;
};
extern Flags global_flags;

void parse_flags(int argc, char * const argv[]);

#endif  // !defined(_FLAGS_H)
