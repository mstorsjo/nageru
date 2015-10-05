#ifndef _PBO_FRAME_ALLOCATOR 
#define _PBO_FRAME_ALLOCATOR 1

#include <epoxy/gl.h>
#include <mutex>
#include <queue>

#include "bmusb.h"

// An allocator that allocates straight into OpenGL pinned memory.
// Meant for video frames only. We use a queue rather than a stack,
// since we want to maximize pipelineability.
class PBOFrameAllocator : public FrameAllocator {
public:
	// Note: You need to have an OpenGL context when calling
	// the constructor.
	PBOFrameAllocator(size_t frame_size,
	                  GLuint width, GLuint height,
	                  size_t num_queued_frames = 16,  // FIXME: should be 6
	                  GLenum buffer = GL_PIXEL_UNPACK_BUFFER_ARB,
	                  GLenum permissions = GL_MAP_WRITE_BIT,
	                  GLenum map_bits = GL_MAP_FLUSH_EXPLICIT_BIT);
	~PBOFrameAllocator() override;
	Frame alloc_frame() override;
	void release_frame(Frame frame) override;

	struct Userdata {
		GLuint pbo;
		GLuint tex_y, tex_cbcr;
	};

private:
	size_t frame_size;

	std::mutex freelist_mutex;
	std::queue<Frame> freelist;
	GLenum buffer;
	std::unique_ptr<Userdata[]> userdata;
};

#endif  // !defined(_PBO_FRAME_ALLOCATOR)
