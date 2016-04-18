#ifndef _METACUBE2_H
#define _METACUBE2_H

/*
 * Definitions for the Metacube2 protocol, used to communicate with Cubemap.
 *
 * Note: This file is meant to compile as both C and C++, for easier inclusion
 * in other projects.
 */

#include <stdint.h>

#define METACUBE2_SYNC "cube!map"  /* 8 bytes long. */
#define METACUBE_FLAGS_HEADER 0x1
#define METACUBE_FLAGS_NOT_SUITABLE_FOR_STREAM_START 0x2

struct metacube2_block_header {
	char sync[8];    /* METACUBE2_SYNC */
	uint32_t size;   /* Network byte order. Does not include header. */
	uint16_t flags;  /* Network byte order. METACUBE_FLAGS_*. */
	uint16_t csum;   /* Network byte order. CRC16 of size and flags. */
};

uint16_t metacube2_compute_crc(const struct metacube2_block_header *hdr);

#endif  /* !defined(_METACUBE_H) */
