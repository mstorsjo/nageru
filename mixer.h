#ifndef _MIXER_H
#define _MIXER_H 1

// The actual video mixer, running in its own separate background thread.

#include <epoxy/gl.h>
#include <functional>

#include "bmusb.h"
#include "ref_counted_gl_sync.h"

#define NUM_CARDS 2

namespace movit {
class Effect;
class ResourcePool;
}
class QOpenGLContext;
class QSurface;

class Mixer {
public:
	void start(QSurface *surface, QSurface *surface2, QSurface *surface3, QSurface *surface4);
	void quit();

	enum Source {
		SOURCE_INPUT1,
		SOURCE_INPUT2,
		SOURCE_SBS,
	};
	void cut(Source source);

	struct DisplayFrame {
		GLuint texnum;
		RefCountedGLsync ready_fence;  // Asserted when the texture is done rendering.
	};
	// Implicitly frees the previous one if there's a new frame available.
	bool get_display_frame(DisplayFrame *frame);

	typedef std::function<void()> new_frame_ready_callback_t;
	void set_frame_ready_fallback(new_frame_ready_callback_t callback);

private:
	void bm_frame(int card_index, uint16_t timecode,
		FrameAllocator::Frame video_frame, size_t video_offset, uint16_t video_format,
		FrameAllocator::Frame audio_frame, size_t audio_offset, uint16_t audio_format);
	void place_rectangle(movit::Effect *resample_effect, movit::Effect *padding_effect, float x0, float y0, float x1, float y1);
	void thread_func(QSurface *surface, QSurface *surface2, QSurface *surface3, QSurface *surface4);

	Source current_source = SOURCE_INPUT1;

	movit::ResourcePool *resource_pool;

	std::mutex display_frame_mutex;
	DisplayFrame current_display_frame, ready_display_frame;  // protected by <frame_mutex>
	bool has_current_display_frame = false, has_ready_display_frame = false;  // protected by <frame_mutex>

	std::mutex bmusb_mutex;
	struct CaptureCard {
		BMUSBCapture *usb;

		// Threading stuff
		bool thread_initialized;
		QSurface *surface;
		QOpenGLContext *context;

		bool new_data_ready;  // Whether new_frame contains anything.
		FrameAllocator::Frame new_frame;
		GLsync new_data_ready_fence;  // Whether new_frame is ready for rendering.
		std::condition_variable new_data_ready_changed;  // Set whenever new_data_ready is changed.
	};
	CaptureCard cards[NUM_CARDS];  // protected by <bmusb_mutex>

	new_frame_ready_callback_t new_frame_ready_callback;
	bool has_new_frame_ready_callback = false;

	std::thread mixer_thread;
	bool should_quit;
};

extern Mixer *global_mixer;

#endif  // !defined(_MIXER_H)
