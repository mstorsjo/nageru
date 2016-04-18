// A wrapper around x264, to encode video in higher quality than Quick Sync
// can give us. We maintain a queue of uncompressed Y'CbCr frames (of 50 frames,
// so a little under 100 MB at 720p), then have a separate thread pull out
// those threads as fast as we can to give it to x264 for encoding.
//
// TODO: We use x264's “speedcontrol” patch if available, so that quality is
// automatically scaled up or down to content and available CPU time.
//
// The encoding threads are niced down because mixing is more important than
// encoding; if we lose frames in mixing, we'll lose frames to disk _and_
// to the stream, as where if we lose frames in encoding, we'll lose frames
// to the stream only, so the latter is strictly better. More importantly,
// this allows speedcontrol (when implemented) to do its thing without
// disturbing the mixer.

#ifndef _X264ENCODE_H
#define _X264ENCODE_H 1

#include <stdint.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <queue>

extern "C" {
#include "x264.h"
}

class Mux;

class X264Encoder {
public:
	X264Encoder(Mux *httpd);  // Does not take ownership.

	// Called after the last frame. Will block; once this returns,
	// the last data is flushed.
	~X264Encoder();

	// <data> is taken to be raw NV12 data of WIDTHxHEIGHT resolution.
	// Does not block.
	void add_frame(int64_t pts, const uint8_t *data);

private:
	struct QueuedFrame {
		int64_t pts;
		uint8_t *data;
	};
	void encoder_thread_func();
	void init_x264();
	void encode_frame(QueuedFrame qf);

	// One big memory chunk of all 50 (or whatever) frames, allocated in
	// the constructor. All data functions just use pointers into this
	// pool.
	std::unique_ptr<uint8_t[]> frame_pool;

	Mux *mux = nullptr;

	std::thread encoder_thread;
	std::atomic<bool> should_quit{false};
	x264_t *x264;

	// Protects everything below it.
	std::mutex mu;

	// Frames that are not being encoded or waiting to be encoded,
	// so that add_frame() can use new ones.
	std::queue<uint8_t *> free_frames;

	// Frames that are waiting to be encoded (ie., add_frame() has been
	// called, but they are not picked up for encoding yet).
	std::queue<QueuedFrame> queued_frames;

	// Whenever the state of <queued_frames> changes.
	std::condition_variable queued_frames_nonempty;
};

#endif  // !defined(_X264ENCODE_H)
