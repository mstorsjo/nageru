#ifndef _DEFS_H
#define _DEFS_H

#define OUTPUT_FREQUENCY 48000
#define MAX_FPS 60
#define WIDTH 1280
#define HEIGHT 720
#define MAX_CARDS 16

// For deinterlacing. See also comments on InputState.
#define FRAME_HISTORY_LENGTH 5

#define AUDIO_OUTPUT_CODEC_NAME "pcm_s32le"
#define AUDIO_OUTPUT_BIT_RATE 0

#define LOCAL_DUMP_PREFIX "record-"
#define LOCAL_DUMP_SUFFIX ".nut"
#define DEFAULT_STREAM_MUX_NAME "nut"  // Only for HTTP. Local dump guesses from LOCAL_DUMP_SUFFIX.
#define MUX_OPTS { \
	/* Make seekable .mov files. */ \
	{ "movflags", "empty_moov+frag_keyframe+default_base_moof" }, \
	\
	/* Keep nut muxer from using unlimited amounts of memory. */ \
	{ "write_index", "0" } \
}

// In bytes. Beware, if too small, stream clients will start dropping data.
// For mov, you want this at 10MB or so (for the reason mentioned above),
// but for nut, there's no flushing, so such a large mux buffer would cause
// the output to be very uneven.
#define MUX_BUFFER_SIZE 65536

#endif  // !defined(_DEFS_H)
