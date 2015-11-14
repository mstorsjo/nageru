#ifndef _DEFS_H
#define _DEFS_H

// OUTPUT_FREQUENCY/FPS must be an integer for now.
#define OUTPUT_FREQUENCY 48000
#define FPS 60

#define AUDIO_OUTPUT_CODEC AV_CODEC_ID_MP3
#define AUDIO_OUTPUT_SAMPLE_FMT AV_SAMPLE_FMT_FLTP
#define AUDIO_OUTPUT_BIT_RATE 256000

#define LOCAL_DUMP_FILE_NAME "test.ts"
#define STREAM_MUX_NAME "mpegts"
#define MUX_OPTS {}

// In bytes. Beware, if too small, stream clients will start dropping data.
#define MUX_BUFFER_SIZE 10485760

#endif  // !defined(_DEFS_H)
