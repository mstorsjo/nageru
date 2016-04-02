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
#include "input_state.h"
#include "correlation_measurer.h"

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
	                           float gain_staging_db, float final_makeup_gain_db,
	                           float correlation)> audio_level_callback_t;
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

	int get_channel_signal(unsigned channel) const
	{
		return theme->get_channel_signal(channel);
	}

	int map_signal(unsigned channel)
	{
		return theme->map_signal(channel);
	}

	unsigned get_audio_source() const
	{
		return audio_source_channel;
	}

	void set_audio_source(unsigned channel)
	{
		audio_source_channel = channel;
	}

	void set_signal_mapping(int signal, int card)
	{
		return theme->set_signal_mapping(signal, card);
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

	void set_locut_enabled(bool enabled)
	{
		locut_enabled = enabled;
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

	void set_gain_staging_db(float gain_db)
	{
		std::unique_lock<std::mutex> lock(compressor_mutex);
		level_compressor_enabled = false;
		gain_staging_db = gain_db;
	}

	void set_gain_staging_auto(bool enabled)
	{
		std::unique_lock<std::mutex> lock(compressor_mutex);
		level_compressor_enabled = enabled;
	}

	void set_final_makeup_gain_db(float gain_db)
	{
		std::unique_lock<std::mutex> lock(compressor_mutex);
		final_makeup_gain_auto = false;
		final_makeup_gain = pow(10.0f, gain_db / 20.0f);
	}

	void set_final_makeup_gain_auto(bool enabled)
	{
		std::unique_lock<std::mutex> lock(compressor_mutex);
		final_makeup_gain_auto = enabled;
	}

	void schedule_cut()
	{
		should_cut = true;
	}

	void reset_meters();

	unsigned get_num_cards() const { return num_cards; }

	std::string get_card_description(unsigned card_index) const {
		assert(card_index < num_cards);
		return cards[card_index].capture->get_description();
	}

	std::map<uint32_t, VideoMode> get_available_video_modes(unsigned card_index) const {
		assert(card_index < num_cards);
		return cards[card_index].capture->get_available_video_modes();
	}

	uint32_t get_current_video_mode(unsigned card_index) const {
		assert(card_index < num_cards);
		return cards[card_index].capture->get_current_video_mode();
	}

	void set_video_mode(unsigned card_index, uint32_t mode) {
		assert(card_index < num_cards);
		cards[card_index].capture->set_video_mode(mode);
	}

	void start_mode_scanning(unsigned card_index);

	std::map<uint32_t, std::string> get_available_video_inputs(unsigned card_index) const {
		assert(card_index < num_cards);
		return cards[card_index].capture->get_available_video_inputs();
	}

	uint32_t get_current_video_input(unsigned card_index) const {
		assert(card_index < num_cards);
		return cards[card_index].capture->get_current_video_input();
	}

	void set_video_input(unsigned card_index, uint32_t input) {
		assert(card_index < num_cards);
		cards[card_index].capture->set_video_input(input);
	}

	std::map<uint32_t, std::string> get_available_audio_inputs(unsigned card_index) const {
		assert(card_index < num_cards);
		return cards[card_index].capture->get_available_audio_inputs();
	}

	uint32_t get_current_audio_input(unsigned card_index) const {
		assert(card_index < num_cards);
		return cards[card_index].capture->get_current_audio_input();
	}

	void set_audio_input(unsigned card_index, uint32_t input) {
		assert(card_index < num_cards);
		cards[card_index].capture->set_audio_input(input);
	}

private:
	void configure_card(unsigned card_index, const QSurfaceFormat &format, CaptureInterface *capture);
	void bm_frame(unsigned card_index, uint16_t timecode,
		FrameAllocator::Frame video_frame, size_t video_offset, VideoFormat video_format,
		FrameAllocator::Frame audio_frame, size_t audio_offset, AudioFormat audio_format);
	void place_rectangle(movit::Effect *resample_effect, movit::Effect *padding_effect, float x0, float y0, float x1, float y1);
	void thread_func();
	void audio_thread_func();
	void process_audio_one_frame(int64_t frame_pts_int, int num_samples);
	void subsample_chroma(GLuint src_tex, GLuint dst_dst);
	void release_display_frame(DisplayFrame *frame);
	double pts() { return double(pts_int) / TIMEBASE; }

	HTTPD httpd;
	unsigned num_cards;

	QSurface *mixer_surface, *h264_encoder_surface;
	std::unique_ptr<movit::ResourcePool> resource_pool;
	std::unique_ptr<Theme> theme;
	std::atomic<unsigned> audio_source_channel{0};
	std::unique_ptr<movit::EffectChain> display_chain;
	GLuint cbcr_program_num;  // Owned by <resource_pool>.
	GLuint cbcr_vbo;  // Holds position and texcoord data.
	GLuint cbcr_position_attribute_index, cbcr_texcoord_attribute_index;
	std::unique_ptr<H264Encoder> h264_encoder;

	// Effects part of <display_chain>. Owned by <display_chain>.
	movit::FlatInput *display_input;

	int64_t pts_int = 0;  // In TIMEBASE units.

	std::mutex bmusb_mutex;
	struct CaptureCard {
		CaptureInterface *capture;
		std::unique_ptr<PBOFrameAllocator> frame_allocator;

		// Stuff for the OpenGL context (for texture uploading).
		QSurface *surface;
		QOpenGLContext *context;

		struct NewFrame {
			RefCountedFrame frame;
			int64_t length;  // In TIMEBASE units.
			bool interlaced;
			unsigned field;  // Which field (0 or 1) of the frame to use. Always 0 for progressive.
			GLsync ready_fence;  // Whether frame is ready for rendering.
			unsigned dropped_frames = 0;  // Number of dropped frames before this one.
		};
		std::queue<NewFrame> new_frames;
		bool should_quit = false;
		std::condition_variable new_frames_changed;  // Set whenever new_frames (or should_quit) is changed.

		// Accumulated errors in number of 1/TIMEBASE samples. If OUTPUT_FREQUENCY divided by
		// frame rate is integer, will always stay zero.
		unsigned fractional_samples = 0;

		std::mutex audio_mutex;
		std::unique_ptr<ResamplingQueue> resampling_queue;  // Under audio_mutex.
		int last_timecode = -1;  // Unwrapped.
		int64_t next_local_pts = 0;  // Beginning of next frame, in TIMEBASE units.
	};
	CaptureCard cards[MAX_CARDS];  // protected by <bmusb_mutex>

	InputState input_state;

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
	std::thread audio_thread;
	std::atomic<bool> should_quit{false};
	std::atomic<bool> should_cut{false};

	audio_level_callback_t audio_level_callback = nullptr;
	std::mutex compressor_mutex;
	Ebu_r128_proc r128;  // Under compressor_mutex.
	CorrelationMeasurer correlation;  // Under compressor_mutex.

	Resampler peak_resampler;
	std::atomic<float> peak{0.0f};

	StereoFilter locut;  // Default cutoff 120 Hz, 24 dB/oct.
	std::atomic<float> locut_cutoff_hz;
	std::atomic<bool> locut_enabled{true};

	// First compressor; takes us up to about -12 dBFS.
	StereoCompressor level_compressor;  // Under compressor_mutex. Used to set/override gain_staging_db if <level_compressor_enabled>.
	float gain_staging_db = 0.0f;  // Under compressor_mutex.
	bool level_compressor_enabled = true;  // Under compressor_mutex.

	static constexpr float ref_level_dbfs = -14.0f;  // Chosen so that we end up around 0 LU in practice.
	static constexpr float ref_level_lufs = -23.0f;  // 0 LU, more or less by definition.

	StereoCompressor limiter;
	std::atomic<float> limiter_threshold_dbfs{ref_level_dbfs + 4.0f};   // 4 dB.
	std::atomic<bool> limiter_enabled{true};
	StereoCompressor compressor;
	std::atomic<float> compressor_threshold_dbfs{ref_level_dbfs - 12.0f};  // -12 dB.
	std::atomic<bool> compressor_enabled{true};

	double final_makeup_gain = 1.0;  // Under compressor_mutex. Read/write by the user. Note: Not in dB, we want the numeric precision so that we can change it slowly.
	bool final_makeup_gain_auto = true;  // Under compressor_mutex.

	std::unique_ptr<ALSAOutput> alsa;

	struct AudioTask {
		int64_t pts_int;
		int num_samples;
	};
	std::mutex audio_mutex;
	std::condition_variable audio_task_queue_changed;
	std::queue<AudioTask> audio_task_queue;  // Under audio_mutex.

	// For mode scanning.
	bool is_mode_scanning[MAX_CARDS]{ false };
	std::vector<uint32_t> mode_scanlist[MAX_CARDS];
	unsigned mode_scanlist_index[MAX_CARDS]{ 0 };
	timespec last_mode_scan_change[MAX_CARDS];
};

extern Mixer *global_mixer;

#endif  // !defined(_MIXER_H)
