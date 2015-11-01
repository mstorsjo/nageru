#ifndef _REF_COUNTED_FRAME_H
#define _REF_COUNTED_FRAME_H 1

// A wrapper around FrameAllocator::Frame that is automatically refcounted;
// when the refcount goes to zero, the frame is given back to the allocator.
//
// Note that the important point isn't really the pointer to the Frame itself,
// it's the resources it's representing that need to go back to the allocator.

#include <memory>

#include "bmusb/bmusb.h"

void release_refcounted_frame(FrameAllocator::Frame *frame);

typedef std::shared_ptr<FrameAllocator::Frame> RefCountedFrameBase;

class RefCountedFrame : public RefCountedFrameBase {
public:
	RefCountedFrame() {}

	RefCountedFrame(const FrameAllocator::Frame &frame)
		: RefCountedFrameBase(new FrameAllocator::Frame(frame), release_refcounted_frame) {}
};

#endif  // !defined(_REF_COUNTED_FRAME_H)
