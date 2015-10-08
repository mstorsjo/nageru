#ifndef _BMUSB_H
#define _BMUSB_H

#include <stdint.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

struct libusb_transfer;

// An interface for frame allocators; if you do not specify one
// (using set_video_frame_allocator), a default one that pre-allocates
// a freelist of eight frames using new[] will be used. Specifying
// your own can be useful if you have special demands for where you want the
// frame to end up and don't want to spend the extra copy to get it there, for
// instance GPU memory.
class FrameAllocator {
 public:
	struct Frame {
		uint8_t *data = nullptr;
		uint8_t *data2 = nullptr;  // Only if interleaved == true.
		size_t len = 0;  // Number of bytes we actually have.
		size_t size = 0;  // Number of bytes we have room for.
		void *userdata = nullptr;
		FrameAllocator *owner = nullptr;

		// If set to true, every other byte will go to data and to data2.
		// If so, <len> and <size> are still about the number of total bytes
		// so if size == 1024, there's 512 bytes in data and 512 in data2.
		bool interleaved = false;
	};

	virtual ~FrameAllocator();

	// Request a video frame. Note that this is called from the
	// USB thread, which runs with realtime priority and is
	// very sensitive to delays. Thus, you should not do anything
	// here that might sleep, including calling malloc().
	// (Taking a mutex is borderline.)
	//
	// The Frame object will be given to the frame callback,
	// which is responsible for releasing the video frame back
	// once it is usable for new frames (ie., it will no longer
	// be read from). You can use the "userdata" pointer for
	// whatever you want to identify this frame if you need to.
	//
	// Returning a Frame with data==nullptr is allowed;
	// if so, the frame in progress will be dropped.
	virtual Frame alloc_frame() = 0;

	virtual void release_frame(Frame frame) = 0;
};

typedef std::function<void(uint16_t timecode,
                           FrameAllocator::Frame video_frame, size_t video_offset, uint16_t video_format,
                           FrameAllocator::Frame audio_frame, size_t audio_offset, uint16_t audio_format)>
	frame_callback_t;

// The actual capturing class, representing capture from a single card.
class BMUSBCapture {
 public:
	BMUSBCapture(int vid = 0x1edb, int pid = 0xbd3b)
		: vid(vid), pid(pid)
	{
	}

	// Does not take ownership.
	void set_video_frame_allocator(FrameAllocator *allocator)
	{
		video_frame_allocator = allocator;
	}

	FrameAllocator *get_video_frame_allocator()
	{
		return video_frame_allocator;
	}

	// Does not take ownership.
	void set_audio_frame_allocator(FrameAllocator *allocator)
	{
		audio_frame_allocator = allocator;
	}

	FrameAllocator *get_audio_frame_allocator()
	{
		return audio_frame_allocator;
	}

	void set_frame_callback(frame_callback_t callback)
	{
		frame_callback = callback;
	}

	// Needs to be run before configure_card().
	void set_dequeue_thread_callbacks(std::function<void()> init, std::function<void()> cleanup)
	{
		dequeue_init_callback = init;
		dequeue_cleanup_callback = cleanup;
		has_dequeue_callbacks = true;
	}

	void configure_card();
	void start_bm_capture();
	void stop_dequeue_thread();

	static void start_bm_thread();
	static void stop_bm_thread();

 private:
	struct QueuedFrame {
		uint16_t timecode;
		uint16_t format;
		FrameAllocator::Frame frame;
	};

	void start_new_audio_block(const uint8_t *start);
	void start_new_frame(const uint8_t *start);

	void queue_frame(uint16_t format, uint16_t timecode, FrameAllocator::Frame frame, std::deque<QueuedFrame> *q);
	void dequeue_thread_func();

	static void usb_thread_func();
	static void cb_xfr(struct libusb_transfer *xfr);

	FrameAllocator::Frame current_video_frame;
	FrameAllocator::Frame current_audio_frame;

	std::mutex queue_lock;
	std::condition_variable queues_not_empty;
	std::deque<QueuedFrame> pending_video_frames;
	std::deque<QueuedFrame> pending_audio_frames;

	FrameAllocator *video_frame_allocator = nullptr;
	FrameAllocator *audio_frame_allocator = nullptr;
	frame_callback_t frame_callback = nullptr;

	std::thread dequeue_thread;
	std::atomic<bool> dequeue_thread_should_quit;
	bool has_dequeue_callbacks = false;
	std::function<void()> dequeue_init_callback = nullptr;
	std::function<void()> dequeue_cleanup_callback = nullptr;

	int current_register = 0;

	static constexpr int NUM_BMUSB_REGISTERS = 60;
	uint8_t register_file[NUM_BMUSB_REGISTERS];

	int vid, pid;
	std::vector<libusb_transfer *> iso_xfrs;
};

#endif
