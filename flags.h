#ifndef _FLAGS_H
#define _FLAGS_H

struct Flags {
	int num_cards = 2;
};
extern Flags global_flags;

void parse_flags(int argc, char * const argv[]);

#endif  // !defined(_FLAGS_H)
