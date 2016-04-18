#include <string.h>
#include <unistd.h>

#include "defs.h"
#include "flags.h"
#include "mux.h"
#include "timebase.h"
#include "x264encode.h"

extern "C" {
#include <libavformat/avformat.h>
}

using namespace std;

X264Encoder::X264Encoder(Mux *mux)
	: mux(mux)
{
	frame_pool.reset(new uint8_t[WIDTH * HEIGHT * 2 * X264_QUEUE_LENGTH]);
	for (unsigned i = 0; i < X264_QUEUE_LENGTH; ++i) {
		free_frames.push(frame_pool.get() + i * (WIDTH * HEIGHT * 2));
	}
	encoder_thread = thread(&X264Encoder::encoder_thread_func, this);
}

X264Encoder::~X264Encoder()
{
	// TODO: close x264
}

void X264Encoder::add_frame(int64_t pts, const uint8_t *data)
{
	QueuedFrame qf;
	qf.pts = pts;

	{
		lock_guard<mutex> lock(mu);
		if (free_frames.empty()) {
			fprintf(stderr, "WARNING: x264 queue full, dropping frame with pts %ld\n", pts);
			return;
		}

		qf.data = free_frames.front();
		free_frames.pop();
	}

	memcpy(qf.data, data, WIDTH * HEIGHT * 2);

	{
		lock_guard<mutex> lock(mu);
		queued_frames.push(qf);
		queued_frames_nonempty.notify_all();
	}
}
	
void X264Encoder::end_encoding()
{
	// TODO
}

void X264Encoder::init_x264()
{
	x264_param_t param;
	x264_param_default_preset(&param, global_flags.x264_preset.c_str(), global_flags.x264_tune.c_str());

	param.i_width = WIDTH;
	param.i_height = HEIGHT;
	param.i_csp = X264_CSP_NV12;
	param.b_vfr_input = 1;
	param.i_timebase_num = 1;
	param.i_timebase_den = TIMEBASE;
	param.i_keyint_max = 50; // About one second.

	// NOTE: These should be in sync with the ones in h264encode.cpp (sbs_rbsp()).
	param.vui.i_vidformat = 5;  // Unspecified.
	param.vui.b_fullrange = 0;
	param.vui.i_colorprim = 1;  // BT.709.
	param.vui.i_transfer = 2;  // Unspecified (since we use sRGB).
	param.vui.i_colmatrix = 6;  // BT.601/SMPTE 170M.

	// 4.5 Mbit/sec, CBR.
	param.rc.i_rc_method = X264_RC_ABR;
	param.rc.i_bitrate = 4500;

	// One-second VBV.
	param.rc.i_vbv_max_bitrate = 4500;
	param.rc.i_vbv_buffer_size = 4500;

	// TODO: more flags here, via x264_param_parse().

	x264_param_apply_profile(&param, "high");

	x264 = x264_encoder_open(&param);
	if (x264 == nullptr) {
		fprintf(stderr, "ERROR: x264 initialization failed.\n");
		exit(1);
	}
}

void X264Encoder::encoder_thread_func()
{
	nice(5);  // Note that x264 further nices some of its threads.
	init_x264();

	for ( ;; ) {
		QueuedFrame qf;

		// Wait for a queued frame, then dequeue it.
		{
			unique_lock<mutex> lock(mu);
			queued_frames_nonempty.wait(lock, [this]() { return !queued_frames.empty(); });
			qf = queued_frames.front();
			queued_frames.pop();
		}

		encode_frame(qf);
		
		{
			lock_guard<mutex> lock(mu);
			free_frames.push(qf.data);
		}
	}
}

void X264Encoder::encode_frame(X264Encoder::QueuedFrame qf)
{
	x264_picture_t pic;
	x264_nal_t *nal = nullptr;
	int num_nal = 0;

	x264_picture_init(&pic);

	// TODO: Delayed frames.
	pic.i_pts = qf.pts;
	pic.img.i_csp = X264_CSP_NV12;
	pic.img.i_plane = 2;
	pic.img.plane[0] = qf.data;
	pic.img.i_stride[0] = WIDTH;
	pic.img.plane[1] = qf.data + WIDTH * HEIGHT;
	pic.img.i_stride[1] = WIDTH / 2 * sizeof(uint16_t);

	x264_encoder_encode(x264, &nal, &num_nal, &pic, &pic);

	// We really need one AVPacket for the entire frame, it seems,
	// so combine it all.
	size_t num_bytes = 0;
	for (int i = 0; i < num_nal; ++i) {
		num_bytes += nal[i].i_payload;
	}

	unique_ptr<uint8_t[]> data(new uint8_t[num_bytes]);
	uint8_t *ptr = data.get();

	for (int i = 0; i < num_nal; ++i) {
		memcpy(ptr, nal[i].p_payload, nal[i].i_payload);
		ptr += nal[i].i_payload;
	}

	AVPacket pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.buf = nullptr;
	pkt.data = data.get();
	pkt.size = num_bytes;
	pkt.stream_index = 0;
	if (pic.b_keyframe) {
		pkt.flags = AV_PKT_FLAG_KEY;
	} else {
		pkt.flags = 0;
	}

	mux->add_packet(pkt, pic.i_pts, pic.i_dts);
}	
