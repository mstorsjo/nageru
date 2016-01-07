// Hardware H.264 encoding via VAAPI. Heavily modified based on example
// code by Intel. Intel's original copyright and license is reproduced below:
//
// Copyright (c) 2007-2013 Intel Corporation. All Rights Reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sub license, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
// 
// The above copyright notice and this permission notice (including the
// next paragraph) shall be included in all copies or substantial portions
// of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
// IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
// ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
// TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
// SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#ifndef _H264ENCODE_H
#define _H264ENCODE_H

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#include <epoxy/gl.h>
#include <stdbool.h>
#include <stdint.h>
#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "bmusb/bmusb.h"
#include "context.h"
#include "pbo_frame_allocator.h"
#include "ref_counted_frame.h"
#include "ref_counted_gl_sync.h"

class HTTPD;
class QSurface;

#define SURFACE_NUM 16 /* 16 surfaces for source YUV */

class H264Encoder {
public:
	H264Encoder(QSurface *surface, int width, int height, HTTPD *httpd);
	~H264Encoder();
	//void add_frame(FrameAllocator::Frame frame, GLsync fence);

#if 0
	struct Frame {
	public:
		GLuint fbo;
		GLuint y_tex, cbcr_tex;	

	private:
		//int surface_subnum;
	};
	void 
#endif
	void add_audio(int64_t pts, std::vector<float> audio);  // Needs to come before end_frame() of same pts.
	bool begin_frame(GLuint *y_tex, GLuint *cbcr_tex);
	void end_frame(RefCountedGLsync fence, int64_t pts, const std::vector<RefCountedFrame> &input_frames);

private:
	struct storage_task {
		unsigned long long display_order;
		int frame_type;
		std::vector<float> audio;
		int64_t pts, dts;
	};
	struct PendingFrame {
		RefCountedGLsync fence;
		std::vector<RefCountedFrame> input_frames;
		int64_t pts;
	};

	void copy_thread_func();
	void encode_frame(PendingFrame frame, int encoding_frame_num, int display_frame_num,
	                  int frame_type, int64_t pts, int64_t dts);
	void storage_task_thread();
	void storage_task_enqueue(storage_task task);
	void save_codeddata(storage_task task);

	std::thread copy_thread, storage_thread;

	std::mutex storage_task_queue_mutex;
	std::condition_variable storage_task_queue_changed;
	int srcsurface_status[SURFACE_NUM];  // protected by storage_task_queue_mutex
	std::queue<storage_task> storage_task_queue;  // protected by storage_task_queue_mutex
	bool storage_thread_should_quit = false;  // protected by storage_task_queue_mutex

	std::mutex frame_queue_mutex;
	std::condition_variable frame_queue_nonempty;
	bool copy_thread_should_quit = false;  // under frame_queue_mutex

	//int frame_width, frame_height;
	//int ;
	int current_storage_frame;

	std::map<int, PendingFrame> pending_video_frames;  // under frame_queue_mutex
	std::map<int64_t, std::vector<float>> pending_audio_frames;  // under frame_queue_mutex
	QSurface *surface;

	AVCodecContext *context_audio;
	HTTPD *httpd;
};

#endif
