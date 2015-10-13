#include "ref_counted_frame.h"

void release_refcounted_frame(FrameAllocator::Frame *frame)
{
	if (frame->owner) {
		frame->owner->release_frame(*frame);
		delete frame;
	}
}
