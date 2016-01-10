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

#include <epoxy/gl.h>
#include <stdint.h>
#include <atomic>
#include <memory>
#include <vector>

#include "ref_counted_frame.h"
#include "ref_counted_gl_sync.h"

class H264EncoderImpl;
class HTTPD;
class QSurface;

// This is just a pimpl, because including anything X11-related in a .h file
// tends to trip up Qt. All the real logic is in H264EncoderImpl, defined in the
// .cpp file.
class H264Encoder {
public:
        H264Encoder(QSurface *surface, int width, int height, HTTPD *httpd);
        ~H264Encoder();

	void add_audio(int64_t pts, std::vector<float> audio);  // Needs to come before end_frame() of same pts.
	bool begin_frame(GLuint *y_tex, GLuint *cbcr_tex);
	void end_frame(RefCountedGLsync fence, int64_t pts, const std::vector<RefCountedFrame> &input_frames);
	void shutdown();  // Blocking.

private:
	std::unique_ptr<H264EncoderImpl> impl;
};

#endif
