#ifndef _INPUT_STATE_H
#define _INPUT_STATE_H 1

#include "defs.h"
#include "ref_counted_frame.h"

struct BufferedFrame {
	RefCountedFrame frame;
	unsigned field_number;
};

// Encapsulates the state of all inputs at any given instant.
// In particular, this is captured by Theme::get_chain(),
// so that it can hold on to all the frames it needs for rendering.
struct InputState {
	// For each card, the last three frames (or fields), with 0 being the
	// most recent one. Note that we only need the actual history if we have
	// interlaced output (for deinterlacing), so if we detect progressive input,
	// we immediately clear out all history and all entries will point to the same
	// frame.
	BufferedFrame buffered_frames[MAX_CARDS][FRAME_HISTORY_LENGTH];
};

#endif  // !defined(_INPUT_STATE_H)
