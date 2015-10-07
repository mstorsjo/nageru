#ifndef _MIXER_H
#define _MIXER_H 1

// The actual video mixer, running in its own separate background thread.

#include <epoxy/gl.h>
#undef Success
#include <movit/effect_chain.h>
#include <movit/flat_input.h>
#include <functional>

#include "bmusb.h"
#include "h264encode.h"
#include "pbo_frame_allocator.h"
#include "ref_counted_frame.h"
#include "ref_counted_gl_sync.h"
#include "theme.h"

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
		OUTPUT_INPUT0,
		OUTPUT_INPUT1,
		NUM_OUTPUTS
	};

	struct DisplayFrame {
		// The chain for rendering this frame. To render a display frame,
		// first wait for <ready_fence>, then call <setup_chain>
		// to wire up all the inputs, and then finally call
		// chain->render_to_screen() or similar.
		movit::EffectChain *chain;
		std::function<void()> setup_chain;

		// Asserted when all the inputs are ready; you cannot render the chain
		// before this.
		RefCountedGLsync ready_fence;

		// Holds on to all the input frames needed for this display frame,
		// so they are not released while still rendering.
		std::vector<RefCountedFrame> input_frames;

		// Textures that should be released back to the resource pool
		// when this frame disappears, if any.
		// TODO: Refcount these as well?
		std::vector<GLuint> temp_textures;
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
	std::unique_ptr<Theme> theme;
	std::unique_ptr<movit::EffectChain> display_chain;
	GLuint cbcr_program_num;  // Owned by <resource_pool>.
	std::unique_ptr<H264Encoder> h264_encoder;

	// Effects part of <display_chain>. Owned by <display_chain>.
	movit::FlatInput *display_input;

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
		void output_frame(DisplayFrame frame);
		bool get_display_frame(DisplayFrame *frame);
		void set_frame_ready_callback(new_frame_ready_callback_t callback);

	private:
		friend class Mixer;

		Mixer *parent = nullptr;  // Not owned.
		std::mutex frame_mutex;
		DisplayFrame current_frame, ready_frame;  // protected by <frame_mutex>
		bool has_current_frame = false, has_ready_frame = false;  // protected by <frame_mutex>
		new_frame_ready_callback_t new_frame_ready_callback;
		bool has_new_frame_ready_callback = false;
	};
	OutputChannel output_channel[NUM_OUTPUTS];

	std::thread mixer_thread;
	bool should_quit = false;
};

extern Mixer *global_mixer;

#endif  // !defined(_MIXER_H)
