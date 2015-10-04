#include "pbo_frame_allocator.h"

#include <stdint.h>
#include <stdio.h>
#include <cstddef>

#include "util.h"

using namespace std;

PBOFrameAllocator::PBOFrameAllocator(size_t frame_size, size_t num_queued_frames, GLenum buffer, GLenum permissions, GLenum map_bits)
        : frame_size(frame_size), buffer(buffer)
{
	for (size_t i = 0; i < num_queued_frames; ++i) {
		GLuint pbo;
		glGenBuffers(1, &pbo);
		check_error();
		glBindBuffer(buffer, pbo);
		check_error();
		glBufferStorage(buffer, frame_size, NULL, permissions | GL_MAP_PERSISTENT_BIT);
		check_error();

		Frame frame;
		frame.data = (uint8_t *)glMapBufferRange(buffer, 0, frame_size, permissions | map_bits | GL_MAP_PERSISTENT_BIT);
		frame.data2 = frame.data + frame_size / 2;
		check_error();
		frame.size = frame_size;
		frame.userdata = (void *)(intptr_t)pbo;
		frame.owner = this;
		frame.interleaved = true;
		freelist.push(frame);
	}
	glBindBuffer(buffer, 0);
	check_error();
}

PBOFrameAllocator::~PBOFrameAllocator()
{
	while (!freelist.empty()) {
		Frame frame = freelist.front();
		freelist.pop();
		GLuint pbo = (intptr_t)frame.userdata;
		glBindBuffer(buffer, pbo);
		check_error();
		glUnmapBuffer(buffer);
		check_error();
		glBindBuffer(buffer, 0);
		check_error();
		glDeleteBuffers(1, &pbo);
	}
}
//static int sumsum = 0;

FrameAllocator::Frame PBOFrameAllocator::alloc_frame()
{
        Frame vf;

	std::unique_lock<std::mutex> lock(freelist_mutex);  // Meh.
	if (freelist.empty()) {
		printf("Frame overrun (no more spare PBO frames), dropping frame!\n");
	} else {
		//fprintf(stderr, "freelist has %d allocated\n", ++sumsum);
		vf = freelist.front();
		freelist.pop();  // Meh.
	}
	vf.len = 0;
	return vf;
}

void PBOFrameAllocator::release_frame(Frame frame)
{
	std::unique_lock<std::mutex> lock(freelist_mutex);
	freelist.push(frame);
	//--sumsum;
}
