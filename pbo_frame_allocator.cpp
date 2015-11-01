#include "pbo_frame_allocator.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <cstddef>

#include "util.h"

using namespace std;

PBOFrameAllocator::PBOFrameAllocator(size_t frame_size, GLuint width, GLuint height, size_t num_queued_frames, GLenum buffer, GLenum permissions, GLenum map_bits)
        : frame_size(frame_size), buffer(buffer)
{
	userdata.reset(new Userdata[num_queued_frames]);
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
		frame.userdata = &userdata[i];
		userdata[i].pbo = pbo;
		frame.owner = this;
		frame.interleaved = true;

		// Create textures.
		glGenTextures(1, &userdata[i].tex_y);
		check_error();
		glBindTexture(GL_TEXTURE_2D, userdata[i].tex_y);
		check_error();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		check_error();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		check_error();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		check_error();
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
		check_error();

		glGenTextures(1, &userdata[i].tex_cbcr);
		check_error();
		glBindTexture(GL_TEXTURE_2D, userdata[i].tex_cbcr);
		check_error();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		check_error();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		check_error();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		check_error();
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, width / 2, height, 0, GL_RG, GL_UNSIGNED_BYTE, NULL);
		check_error();

		freelist.push(frame);
	}
	glBindBuffer(buffer, 0);
	check_error();
	glBindTexture(GL_TEXTURE_2D, 0);
	check_error();
}

PBOFrameAllocator::~PBOFrameAllocator()
{
	while (!freelist.empty()) {
		Frame frame = freelist.front();
		freelist.pop();
		GLuint pbo = ((Userdata *)frame.userdata)->pbo;
		glBindBuffer(buffer, pbo);
		check_error();
		glUnmapBuffer(buffer);
		check_error();
		glBindBuffer(buffer, 0);
		check_error();
		glDeleteBuffers(1, &pbo);
		check_error();
		GLuint tex_y = ((Userdata *)frame.userdata)->tex_y;
		glDeleteTextures(1, &tex_y);
		check_error();
		GLuint tex_cbcr = ((Userdata *)frame.userdata)->tex_cbcr;
		glDeleteTextures(1, &tex_cbcr);
		check_error();
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
	vf.overflow = 0;
	return vf;
}

void PBOFrameAllocator::release_frame(Frame frame)
{
	if (frame.overflow > 0) {
		printf("%d bytes overflow after last (PBO) frame\n", int(frame.overflow));
	}

	std::unique_lock<std::mutex> lock(freelist_mutex);
	freelist.push(frame);
	//--sumsum;
}
