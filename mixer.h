#ifndef _MIXER_H
#define _MIXER_H 1

// The actual video mixer, running in its own separate background thread.

#include <epoxy/gl.h>
#undef Success
#include <stdbool.h>
#include <stdint.h>

#include <movit/effect_chain.h>
#include <movit/flat_input.h>
#include <zita-resampler/resampler.h>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bmusb/bmusb.h"
#include "alsa_output.h"
#include "ebu_r128_proc.h"
#include "h264encode.h"
#include "httpd.h"
#include "pbo_frame_allocator.h"
#include "ref_counted_frame.h"
#include "ref_counted_gl_sync.h"
#include "resampling_queue.h"
#include "theme.h"
#include "timebase.h"
#include "stereocompressor.h"
#include "filter.h"

class H264Encoder;
class QSurface;
namespace movit {
class Effect;
class EffectChain;
class FlatInput;
class ResourcePool;
}  // namespace movit

namespace movit {
class YCbCrInput;
}
class QOpenGLContext;
class QSurfaceFormat;

class Mixer {
public:
	// The surface format is used for offscreen destinations for OpenGL contexts we need.
	Mixer(const QSurfaceFormat &format, unsigned num_cards);
	~Mixer();
	void start();
	void quit();

	void transition_clicked(int transition_num);
	void channel_clicked(int preview_num);

	enum Output {
		OUTPUT_LIVE = 0,
		OUTPUT_PREVIEW,
		OUTPUT_INPUT0,  // 1, 2, 3, up to 15 follow numerically.
		NUM_OUTPUTS = 18
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

	typedef std::function<void(float level_lufs, float peak_db,
	                           float global_level_lufs, float range_low_lufs, float range_high_lufs,
	                           float auto_gain_staging_db)> audio_level_callback_t;
	void set_audio_level_callback(audio_level_callback_t callback)
	{
		audio_level_callback = callback;
	}

	std::vector<std::string> get_transition_names()
	{
		return theme->get_transition_names(pts());
	}

	unsigned get_num_channels() const
	{
		return theme->get_num_channels();
	}

	std::string get_channel_name(unsigned channel) const
	{
		return theme->get_channel_name(channel);
	}

	bool get_supports_set_wb(unsigned channel) const
	{
		return theme->get_supports_set_wb(channel);
	}

	void set_wb(unsigned channel, double r, double g, double b) const
	{
		theme->set_wb(channel, r, g, b);
	}

	void set_locut_cutoff(float cutoff_hz)
	{
		locut_cutoff_hz = cutoff_hz;
	}

	float get_limiter_threshold_dbfs()
	{
		return limiter_threshold_dbfs;
	}

	float get_compressor_threshold_dbfs()
	{
		return compressor_threshold_dbfs;
	}

	void set_limiter_threshold_dbfs(float threshold_dbfs)
	{
		limiter_threshold_dbfs = threshold_dbfs;
	}

	void set_compressor_threshold_dbfs(float threshold_dbfs)
	{
		compressor_threshold_dbfs = threshold_dbfs;
	}

	void set_limiter_enabled(bool enabled)
	{
		limiter_enabled = enabled;
	}

	void set_compressor_enabled(bool enabled)
	{
		compressor_enabled = enabled;
	}

	void reset_meters();

private:
	void bm_frame(unsigned card_index, uint16_t timecode,
		FrameAllocator::Frame video_frame, size_t video_offset, uint16_t video_format,
		FrameAllocator::Frame audio_frame, size_t audio_offset, uint16_t audio_format);
	void place_rectangle(movit::Effect *resample_effect, movit::Effect *padding_effect, float x0, float y0, float x1, float y1);
	void thread_func();
	void process_audio_one_frame();
	void subsample_chroma(GLuint src_tex, GLuint dst_dst);
	void release_display_frame(DisplayFrame *frame);
	double pts() { return double(pts_int) / TIMEBASE; }

	HTTPD httpd;
	unsigned num_cards;

	QSurface *mixer_surface, *h264_encoder_surface;
	std::unique_ptr<movit::ResourcePool> resource_pool;
	std::unique_ptr<Theme> theme;
	std::unique_ptr<movit::EffectChain> display_chain;
	GLuint cbcr_program_num;  // Owned by <resource_pool>.
	std::unique_ptr<H264Encoder> h264_encoder;

	// Effects part of <display_chain>. Owned by <display_chain>.
	movit::FlatInput *display_input;

	int64_t pts_int = 0;  // In TIMEBASE units.

	std::mutex bmusb_mutex;
	struct CaptureCard {
		BMUSBCapture *usb;
		std::unique_ptr<PBOFrameAllocator> frame_allocator;

		// Stuff for the OpenGL context (for texture uploading).
		QSurface *surface;
		QOpenGLContext *context;

		bool new_data_ready = false;  // Whether new_frame contains anything.
		bool should_quit = false;
		RefCountedFrame new_frame;
		GLsync new_data_ready_fence;  // Whether new_frame is ready for rendering.
		std::condition_variable new_data_ready_changed;  // Set whenever new_data_ready is changed.
		unsigned dropped_frames = 0;  // Before new_frame.

		std::mutex audio_mutex;
		std::unique_ptr<ResamplingQueue> resampling_queue;  // Under audio_mutex.
		int last_timecode = -1;  // Unwrapped.
	};
	CaptureCard cards[MAX_CARDS];  // protected by <bmusb_mutex>

	RefCountedFrame bmusb_current_rendering_frame[MAX_CARDS];

	class OutputChannel {
	public:
		~OutputChannel();
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

	audio_level_callback_t audio_level_callback = nullptr;
	Ebu_r128_proc r128;

	Resampler peak_resampler;
	std::atomic<float> peak{0.0f};

	StereoFilter locut;  // Default cutoff 150 Hz, 24 dB/oct.
	std::atomic<float> locut_cutoff_hz;

	// First compressor; takes us up to about -12 dBFS.
	StereoCompressor level_compressor;
	float last_gain_staging_db = 0.0f;

	static constexpr float ref_level_dbfs = -14.0f;

	StereoCompressor limiter;
	std::atomic<float> limiter_threshold_dbfs{ref_level_dbfs + 4.0f};   // 4 dB.
	std::atomic<bool> limiter_enabled{true};
	StereoCompressor compressor;
	std::atomic<float> compressor_threshold_dbfs{ref_level_dbfs - 12.0f};  // -12 dB.
	std::atomic<bool> compressor_enabled{true};

	std::unique_ptr<ALSAOutput> alsa;
};

extern Mixer *global_mixer;

#endif  // !defined(_MIXER_H)
