#ifndef _MIXER_H
#define _MIXER_H 1

// The actual video mixer, running in its own separate background thread.

#include <epoxy/gl.h>
#undef Success
#include <movit/effect_chain.h>
#include <functional>

#include "bmusb.h"
#include "h264encode.h"
#include "pbo_frame_allocator.h"
#include "ref_counted_frame.h"
#include "ref_counted_gl_sync.h"

#define NUM_CARDS 2

namespace movit {
class YCbCrInput;
}
class QOpenGLContext;
class QSurfaceFormat;

class Mixer {
public:
	// The surface format is used for offscreen destinations for OpenGL contexts we need.
	Mixer(const QSurfaceFormat &format);
	~Mixer();
	void start();
	void quit();

	enum Source {
		SOURCE_INPUT1,
		SOURCE_INPUT2,
		SOURCE_SBS,
	};
	void cut(Source source);

	enum Output {
		OUTPUT_LIVE = 0,
		OUTPUT_PREVIEW,
		NUM_OUTPUTS
	};

	struct DisplayFrame {
		GLuint texnum;
		RefCountedGLsync ready_fence;  // Asserted when the texture is done rendering.
	};
	// Implicitly frees the previous one if there's a new frame available.
	bool get_display_frame(Output output, DisplayFrame *frame) {
		return output_channel[output].get_display_frame(frame);
	}

	typedef std::function<void()> new_frame_ready_callback_t;
	void set_frame_ready_callback(Output output, new_frame_ready_callback_t callback)
	{
		output_channel[output].set_frame_ready_callback(callback);
	}

	// Ignored for OUTPUT_LIVE.
	void set_preview_size(Output output, int width, int height)
	{
		output_channel[output].set_size(width, height);
	}

private:
	void bm_frame(int card_index, uint16_t timecode,
		FrameAllocator::Frame video_frame, size_t video_offset, uint16_t video_format,
		FrameAllocator::Frame audio_frame, size_t audio_offset, uint16_t audio_format);
	void place_rectangle(movit::Effect *resample_effect, movit::Effect *padding_effect, float x0, float y0, float x1, float y1);
	void thread_func();
	void subsample_chroma(GLuint src_tex, GLuint dst_dst);
	void release_display_frame(DisplayFrame *frame);

	QSurface *mixer_surface, *h264_encoder_surface;
	std::unique_ptr<movit::ResourcePool> resource_pool;
	std::unique_ptr<movit::EffectChain> chain;
	std::unique_ptr<movit::EffectChain> preview_chain;
	GLuint cbcr_program_num;  // Owned by <resource_pool>.
	std::unique_ptr<H264Encoder> h264_encoder;

	// Effects part of <chain>. Owned by <chain>.
	movit::YCbCrInput *input[NUM_CARDS];
	movit::Effect *resample_effect, *resample2_effect;
	movit::Effect *padding_effect, *padding2_effect;

	// Effects part of <preview_chain>. Owned by <preview_chain>.
	movit::YCbCrInput *preview_input;

	Source current_source = SOURCE_INPUT1;
	int frame = 0;

	std::mutex bmusb_mutex;
	struct CaptureCard {
		BMUSBCapture *usb;
		std::unique_ptr<PBOFrameAllocator> frame_allocator;

		// Threading stuff
		bool thread_initialized = false;
		QSurface *surface;
		QOpenGLContext *context;

		bool new_data_ready = false;  // Whether new_frame contains anything.
		RefCountedFrame new_frame;
		GLsync new_data_ready_fence;  // Whether new_frame is ready for rendering.
		std::condition_variable new_data_ready_changed;  // Set whenever new_data_ready is changed.
	};
	CaptureCard cards[NUM_CARDS];  // protected by <bmusb_mutex>

	RefCountedFrame bmusb_current_rendering_frame[NUM_CARDS];

	class OutputChannel {
	public:
		void output_frame(GLuint tex, RefCountedGLsync fence);
		bool get_display_frame(DisplayFrame *frame);
		void set_frame_ready_callback(new_frame_ready_callback_t callback);
		void set_size(int width, int height);  // Ignored for OUTPUT_LIVE.

	private:
		friend class Mixer;

		Mixer *parent = nullptr;  // Not owned.
		std::mutex frame_mutex;
		DisplayFrame current_frame, ready_frame;  // protected by <frame_mutex>
		bool has_current_frame = false, has_ready_frame = false;  // protected by <frame_mutex>
		new_frame_ready_callback_t new_frame_ready_callback;
		bool has_new_frame_ready_callback = false;

		int width = 1280, height = 720;
	};
	OutputChannel output_channel[NUM_OUTPUTS];

	std::thread mixer_thread;
	bool should_quit = false;
};

extern Mixer *global_mixer;

#endif  // !defined(_MIXER_H)
