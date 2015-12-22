#ifndef _DEFS_H
#define _DEFS_H

#define OUTPUT_FREQUENCY 48000
#define MAX_FPS 60
#define WIDTH 1280
#define HEIGHT 720
#define MAX_CARDS 16

// For deinterlacing. See also comments on InputState.
#define FRAME_HISTORY_LENGTH 5

#define AUDIO_OUTPUT_CODEC AV_CODEC_ID_PCM_S32LE
#define AUDIO_OUTPUT_SAMPLE_FMT AV_SAMPLE_FMT_S32
#define AUDIO_OUTPUT_BIT_RATE 0

#define LOCAL_DUMP_FILE_NAME "test.mov"
#define STREAM_MUX_NAME "mov"
#define MUX_OPTS {{ "movflags", "empty_moov+frag_keyframe+default_base_moof" }}

// In bytes. Beware, if too small, stream clients will start dropping data.
#define MUX_BUFFER_SIZE 10485760

#endif  // !defined(_DEFS_H)
