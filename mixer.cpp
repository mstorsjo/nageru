#define WIDTH 1280
#define HEIGHT 720
#define EXTRAHEIGHT 30

#undef Success

#include "mixer.h"

#include <assert.h>
#include <epoxy/egl.h>
#include <init.h>
#include <movit/effect_chain.h>
#include <movit/effect_util.h>
#include <movit/flat_input.h>
#include <movit/image_format.h>
#include <movit/resource_pool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <util.h>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "bmusb/bmusb.h"
#include "context.h"
#include "h264encode.h"
#include "pbo_frame_allocator.h"
#include "ref_counted_gl_sync.h"
#include "timebase.h"

class QOpenGLContext;

using namespace movit;
using namespace std;
using namespace std::placeholders;

Mixer *global_mixer = nullptr;

namespace {

void convert_fixed24_to_fp32(float *dst, size_t out_channels, const uint8_t *src, size_t in_channels, size_t num_samples)
{
	for (size_t i = 0; i < num_samples; ++i) {
		for (size_t j = 0; j < out_channels; ++j) {
			uint32_t s1 = *src++;
			uint32_t s2 = *src++;
			uint32_t s3 = *src++;
			uint32_t s = s1 | (s1 << 8) | (s2 << 16) | (s3 << 24);
			dst[i * out_channels + j] = int(s) * (1.0f / 4294967296.0f);
		}
		src += 3 * (in_channels - out_channels);
	}
}

}  // namespace

Mixer::Mixer(const QSurfaceFormat &format, unsigned num_cards)
	: httpd("test.ts", WIDTH, HEIGHT),
	  num_cards(num_cards),
	  mixer_surface(create_surface(format)),
	  h264_encoder_surface(create_surface(format))
{
	httpd.start(9095);

	CHECK(init_movit(MOVIT_SHADER_DIR, MOVIT_DEBUG_OFF));
	check_error();

	// Since we allow non-bouncing 4:2:2 YCbCrInputs, effective subpixel precision
	// will be halved when sampling them, and we need to compensate here.
	movit_texel_subpixel_precision /= 2.0;

	resource_pool.reset(new ResourcePool);
	theme.reset(new Theme("theme.lua", resource_pool.get(), num_cards));
	for (unsigned i = 0; i < NUM_OUTPUTS; ++i) {
		output_channel[i].parent = this;
	}

	ImageFormat inout_format;
	inout_format.color_space = COLORSPACE_sRGB;
	inout_format.gamma_curve = GAMMA_sRGB;

	// Display chain; shows the live output produced by the main chain (its RGBA version).
	display_chain.reset(new EffectChain(WIDTH, HEIGHT, resource_pool.get()));
	check_error();
	display_input = new FlatInput(inout_format, FORMAT_RGB, GL_UNSIGNED_BYTE, WIDTH, HEIGHT);  // FIXME: GL_UNSIGNED_BYTE is really wrong.
	display_chain->add_input(display_input);
	display_chain->add_output(inout_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED);
	display_chain->set_dither_bits(0);  // Don't bother.
	display_chain->finalize();

	h264_encoder.reset(new H264Encoder(h264_encoder_surface, WIDTH, HEIGHT, &httpd));

	for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
		printf("Configuring card %d...\n", card_index);
		CaptureCard *card = &cards[card_index];
		card->usb = new BMUSBCapture(card_index);
		card->usb->set_frame_callback(bind(&Mixer::bm_frame, this, card_index, _1, _2, _3, _4, _5, _6, _7));
		card->frame_allocator.reset(new PBOFrameAllocator(WIDTH * (HEIGHT+EXTRAHEIGHT) * 2 + 44, WIDTH, HEIGHT));
		card->usb->set_video_frame_allocator(card->frame_allocator.get());
		card->surface = create_surface(format);
		card->usb->set_dequeue_thread_callbacks(
			[card]{
				eglBindAPI(EGL_OPENGL_API);
				card->context = create_context(card->surface);
				if (!make_current(card->context, card->surface)) {
					printf("failed to create bmusb context\n");
					exit(1);
				}
			},
			[this]{
				resource_pool->clean_context();
			});
		card->resampler.reset(new Resampler(48000.0, 48000.0, 2));
		card->usb->configure_card();
	}

	BMUSBCapture::start_bm_thread();

	for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
		cards[card_index].usb->start_bm_capture();
	}

	//chain->enable_phase_timing(true);

	// Set up stuff for NV12 conversion.

	// Cb/Cr shader.
	string cbcr_vert_shader = read_file("vs-cbcr.130.vert");
	string cbcr_frag_shader =
		"#version 130 \n"
		"in vec2 tc0; \n"
		"uniform sampler2D cbcr_tex; \n"
		"void main() { \n"
		"    gl_FragColor = texture2D(cbcr_tex, tc0); \n"
		"} \n";
	cbcr_program_num = resource_pool->compile_glsl_program(cbcr_vert_shader, cbcr_frag_shader);

	r128.init(2, 48000);
	r128.integr_start();
}

Mixer::~Mixer()
{
	resource_pool->release_glsl_program(cbcr_program_num);
	BMUSBCapture::stop_bm_thread();

	for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
		{
			unique_lock<mutex> lock(bmusb_mutex);
			cards[card_index].should_quit = true;  // Unblock thread.
			cards[card_index].new_data_ready_changed.notify_all();
		}
		cards[card_index].usb->stop_dequeue_thread();
	}
}

namespace {

int unwrap_timecode(uint16_t current_wrapped, int last)
{
	uint16_t last_wrapped = last & 0xffff;
	if (current_wrapped > last_wrapped) {
		return (last & ~0xffff) | current_wrapped;
	} else {
		return 0x10000 + ((last & ~0xffff) | current_wrapped);
	}
}

float find_peak(const vector<float> &samples)
{
	float m = fabs(samples[0]);
	for (size_t i = 1; i < samples.size(); ++i) {
		m = std::max(m, fabs(samples[i]));
	}
	return m;
}

void deinterleave_samples(const vector<float> &in, vector<float> *out_l, vector<float> *out_r)
{
	size_t num_samples = in.size() / 2;
	out_l->resize(num_samples);
	out_r->resize(num_samples);

	const float *inptr = in.data();
	float *lptr = &(*out_l)[0];
	float *rptr = &(*out_r)[0];
	for (size_t i = 0; i < num_samples; ++i) {
		*lptr++ = *inptr++;
		*rptr++ = *inptr++;
	}
}

}  // namespace

void Mixer::bm_frame(unsigned card_index, uint16_t timecode,
                     FrameAllocator::Frame video_frame, size_t video_offset, uint16_t video_format,
		     FrameAllocator::Frame audio_frame, size_t audio_offset, uint16_t audio_format)
{
	CaptureCard *card = &cards[card_index];

	if (audio_frame.len - audio_offset > 30000) {
		printf("Card %d: Dropping frame with implausible audio length (len=%d, offset=%d) [timecode=0x%04x video_len=%d video_offset=%d video_format=%x)\n",
			card_index, int(audio_frame.len), int(audio_offset),
			timecode, int(video_frame.len), int(video_offset), video_format);
		if (video_frame.owner) {
			video_frame.owner->release_frame(video_frame);
		}
		if (audio_frame.owner) {
			audio_frame.owner->release_frame(audio_frame);
		}
		return;
	}

	int unwrapped_timecode = timecode;
	int dropped_frames = 0;
	if (card->last_timecode != -1) {
		unwrapped_timecode = unwrap_timecode(unwrapped_timecode, card->last_timecode);
		dropped_frames = unwrapped_timecode - card->last_timecode - 1;
	}
	card->last_timecode = unwrapped_timecode;

	// Convert the audio to stereo fp32 and add it.
	size_t num_samples = (audio_frame.len >= audio_offset) ? (audio_frame.len - audio_offset) / 8 / 3 : 0;
	vector<float> audio;
	audio.resize(num_samples * 2);
	convert_fixed24_to_fp32(&audio[0], 2, audio_frame.data + audio_offset, 8, num_samples);

	// Add the audio.
	{
		unique_lock<mutex> lock(card->audio_mutex);

		int unwrapped_timecode = timecode;
		if (dropped_frames > 60 * 2) {
			fprintf(stderr, "Card %d lost more than two seconds (or time code jumping around), resetting resampler\n",
				card_index);
			card->resampler.reset(new Resampler(48000.0, 48000.0, 2));
		} else if (dropped_frames > 0) {
			// Insert silence as needed.
			fprintf(stderr, "Card %d dropped %d frame(s) (before timecode 0x%04x), inserting silence.\n",
				card_index, dropped_frames, timecode);
			vector<float> silence;
			silence.resize((48000 / 60) * 2);
			for (int i = 0; i < dropped_frames; ++i) {
				card->resampler->add_input_samples((unwrapped_timecode - dropped_frames + i) / 60.0, silence.data(), (48000 / 60));
			}
		}
		card->resampler->add_input_samples(unwrapped_timecode / 60.0, audio.data(), num_samples);
	}

	// Done with the audio, so release it.
	if (audio_frame.owner) {
		audio_frame.owner->release_frame(audio_frame);
	}

	{
		// Wait until the previous frame was consumed.
		unique_lock<mutex> lock(bmusb_mutex);
		card->new_data_ready_changed.wait(lock, [card]{ return !card->new_data_ready || card->should_quit; });
		if (card->should_quit) return;
	}

	if (video_frame.len - video_offset != WIDTH * (HEIGHT+EXTRAHEIGHT) * 2) {
		if (video_frame.len != 0) {
			printf("Card %d: Dropping video frame with wrong length (%ld)\n",
				card_index, video_frame.len - video_offset);
		}
		if (video_frame.owner) {
			video_frame.owner->release_frame(video_frame);
		}

		// Still send on the information that we _had_ a frame, even though it's corrupted,
		// so that pts can go up accordingly.
		{
			unique_lock<mutex> lock(bmusb_mutex);
			card->new_data_ready = true;
			card->new_frame = RefCountedFrame(FrameAllocator::Frame());
			card->new_data_ready_fence = nullptr;
			card->dropped_frames = dropped_frames;
			card->new_data_ready_changed.notify_all();
		}
		return;
	}

	const PBOFrameAllocator::Userdata *userdata = (const PBOFrameAllocator::Userdata *)video_frame.userdata;
	GLuint pbo = userdata->pbo;
	check_error();
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER_ARB, pbo);
	check_error();
	glFlushMappedBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, video_frame.size);
	check_error();
	//glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
	//check_error();

	// Upload the textures.
	glBindTexture(GL_TEXTURE_2D, userdata->tex_y);
	check_error();
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, WIDTH, HEIGHT, GL_RED, GL_UNSIGNED_BYTE, BUFFER_OFFSET((WIDTH * (HEIGHT+EXTRAHEIGHT) * 2 + 44) / 2 + WIDTH * 25 + 22));
	check_error();
	glBindTexture(GL_TEXTURE_2D, userdata->tex_cbcr);
	check_error();
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, WIDTH/2, HEIGHT, GL_RG, GL_UNSIGNED_BYTE, BUFFER_OFFSET(WIDTH * 25 + 22));
	check_error();
	glBindTexture(GL_TEXTURE_2D, 0);
	check_error();
	GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, /*flags=*/0);              
	check_error();
	assert(fence != nullptr);

	{
		unique_lock<mutex> lock(bmusb_mutex);
		card->new_data_ready = true;
		card->new_frame = RefCountedFrame(video_frame);
		card->new_data_ready_fence = fence;
		card->dropped_frames = dropped_frames;
		card->new_data_ready_changed.notify_all();
	}
}

void Mixer::thread_func()
{
	eglBindAPI(EGL_OPENGL_API);
	QOpenGLContext *context = create_context(mixer_surface);
	if (!make_current(context, mixer_surface)) {
		printf("oops\n");
		exit(1);
	}

	struct timespec start, now;
	clock_gettime(CLOCK_MONOTONIC, &start);

	int frame = 0;
	int dropped_frames = 0;

	while (!should_quit) {
		CaptureCard card_copy[MAX_CARDS];

		{
			unique_lock<mutex> lock(bmusb_mutex);

			// The first card is the master timer, so wait for it to have a new frame.
			// TODO: Make configurable, and with a timeout.
			cards[0].new_data_ready_changed.wait(lock, [this]{ return cards[0].new_data_ready; });

			for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
				CaptureCard *card = &cards[card_index];
				card_copy[card_index].usb = card->usb;
				card_copy[card_index].new_data_ready = card->new_data_ready;
				card_copy[card_index].new_frame = card->new_frame;
				card_copy[card_index].new_data_ready_fence = card->new_data_ready_fence;
				card_copy[card_index].new_frame_audio = move(card->new_frame_audio);
				card_copy[card_index].dropped_frames = card->dropped_frames;
				card->new_data_ready = false;
				card->new_data_ready_changed.notify_all();
			}
		}

		// Resample the audio as needed, including from previously dropped frames.
		vector<float> samples_out;
		// TODO: Allow using audio from the other card(s) as well.
		for (unsigned frame_num = 0; frame_num < card_copy[0].dropped_frames + 1; ++frame_num) {
			for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
				samples_out.resize((48000 / 60) * 2);
				{
					unique_lock<mutex> lock(cards[card_index].audio_mutex);
					if (!cards[card_index].resampler->get_output_samples(pts(), &samples_out[0], 48000 / 60)) {
						printf("Card %d reported previous underrun.\n", card_index);
					}
				}
				if (card_index == 0) {
					vector<float> left, right;
					peak = std::max(peak, find_peak(samples_out));
					deinterleave_samples(samples_out, &left, &right);
					float *ptrs[] = { left.data(), right.data() };
					r128.process(left.size(), ptrs);
					h264_encoder->add_audio(pts_int, move(samples_out));
				}
			}
			if (frame_num != card_copy[0].dropped_frames) {
				// For dropped frames, increase the pts.
				++dropped_frames;
				pts_int += TIMEBASE / 60;
			}
		}

		if (audio_level_callback != nullptr) {
			double loudness_s = r128.loudness_S();
			double loudness_i = r128.integrated();
			double loudness_range_low = r128.range_min();
			double loudness_range_high = r128.range_max();

			audio_level_callback(loudness_s, 20.0 * log10(peak),
			                     loudness_i, loudness_range_low, loudness_range_high);
		}

		for (unsigned card_index = 1; card_index < num_cards; ++card_index) {
			if (card_copy[card_index].new_data_ready && card_copy[card_index].new_frame->len == 0) {
				++card_copy[card_index].dropped_frames;
			}
			if (card_copy[card_index].dropped_frames > 0) {
				printf("Card %u dropped %d frames before this\n",
					card_index, int(card_copy[card_index].dropped_frames));
			}
		}

		// If the first card is reporting a corrupted or otherwise dropped frame,
		// just increase the pts (skipping over this frame) and don't try to compute anything new.
		if (card_copy[0].new_frame->len == 0) {
			++dropped_frames;
			pts_int += TIMEBASE / 60;
			continue;
		}

		for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
			CaptureCard *card = &card_copy[card_index];
			if (!card->new_data_ready || card->new_frame->len == 0)
				continue;

			assert(card->new_frame != nullptr);
			bmusb_current_rendering_frame[card_index] = card->new_frame;
			check_error();

			// The new texture might still be uploaded,
			// tell the GPU to wait until it's there.
			if (card->new_data_ready_fence) {
				glWaitSync(card->new_data_ready_fence, /*flags=*/0, GL_TIMEOUT_IGNORED);
				check_error();
				glDeleteSync(card->new_data_ready_fence);
				check_error();
			}
			const PBOFrameAllocator::Userdata *userdata = (const PBOFrameAllocator::Userdata *)card->new_frame->userdata;
			theme->set_input_textures(card_index, userdata->tex_y, userdata->tex_cbcr);
		}

		// Get the main chain from the theme, and set its state immediately.
		pair<EffectChain *, function<void()>> theme_main_chain = theme->get_chain(0, pts(), WIDTH, HEIGHT);
		EffectChain *chain = theme_main_chain.first;
		theme_main_chain.second();

		GLuint y_tex, cbcr_tex;
		bool got_frame = h264_encoder->begin_frame(&y_tex, &cbcr_tex);
		assert(got_frame);

		// Render main chain.
		GLuint cbcr_full_tex = resource_pool->create_2d_texture(GL_RG8, WIDTH, HEIGHT);
		GLuint rgba_tex = resource_pool->create_2d_texture(GL_RGB565, WIDTH, HEIGHT);  // Saves texture bandwidth, although dithering gets messed up.
		GLuint fbo = resource_pool->create_fbo(y_tex, cbcr_full_tex, rgba_tex);
		check_error();
		chain->render_to_fbo(fbo, WIDTH, HEIGHT);
		resource_pool->release_fbo(fbo);

		subsample_chroma(cbcr_full_tex, cbcr_tex);
		resource_pool->release_2d_texture(cbcr_full_tex);

		// Set the right state for rgba_tex.
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glBindTexture(GL_TEXTURE_2D, rgba_tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		RefCountedGLsync fence(GL_SYNC_GPU_COMMANDS_COMPLETE, /*flags=*/0);
		check_error();

		// Make sure the H.264 gets a reference to all the
		// input frames needed, so that they are not released back
		// until the rendering is done.
		vector<RefCountedFrame> input_frames;
		for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
			input_frames.push_back(bmusb_current_rendering_frame[card_index]);
		}
		const int64_t av_delay = TIMEBASE / 10;  // Corresponds to the fixed delay in resampler.h. TODO: Make less hard-coded.
		h264_encoder->end_frame(fence, pts_int + av_delay, input_frames);
		++frame;
		pts_int += TIMEBASE / 60;

		// The live frame just shows the RGBA texture we just rendered.
		// It owns rgba_tex now.
		DisplayFrame live_frame;
		live_frame.chain = display_chain.get();
		live_frame.setup_chain = [this, rgba_tex]{
			display_input->set_texture_num(rgba_tex);
		};
		live_frame.ready_fence = fence;
		live_frame.input_frames = {};
		live_frame.temp_textures = { rgba_tex };
		output_channel[OUTPUT_LIVE].output_frame(live_frame);

		// Set up preview and any additional channels.
		for (int i = 1; i < theme->get_num_channels() + 2; ++i) {
			DisplayFrame display_frame;
			pair<EffectChain *, function<void()>> chain = theme->get_chain(i, pts(), WIDTH, HEIGHT);  // FIXME: dimensions
			display_frame.chain = chain.first;
			display_frame.setup_chain = chain.second;
			display_frame.ready_fence = fence;

			// FIXME: possible to do better?
			for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
				display_frame.input_frames.push_back(bmusb_current_rendering_frame[card_index]);
			}
			display_frame.temp_textures = {};
			output_channel[i].output_frame(display_frame);
		}

		clock_gettime(CLOCK_MONOTONIC, &now);
		double elapsed = now.tv_sec - start.tv_sec +
			1e-9 * (now.tv_nsec - start.tv_nsec);
		if (frame % 100 == 0) {
			printf("%d frames (%d dropped) in %.3f seconds = %.1f fps (%.1f ms/frame)\n",
				frame, dropped_frames, elapsed, frame / elapsed,
				1e3 * elapsed / frame);
		//	chain->print_phase_timing();
		}

#if 0
		// Reset every 100 frames, so that local variations in frame times
		// (especially for the first few frames, when the shaders are
		// compiled etc.) don't make it hard to measure for the entire
		// remaining duration of the program.
		if (frame == 10000) {
			frame = 0;
			start = now;
		}
#endif
		check_error();
	}

	resource_pool->clean_context();
}

void Mixer::subsample_chroma(GLuint src_tex, GLuint dst_tex)
{
	GLuint vao;
	glGenVertexArrays(1, &vao);
	check_error();

	float vertices[] = {
		0.0f, 2.0f,
		0.0f, 0.0f,
		2.0f, 0.0f
	};

	glBindVertexArray(vao);
	check_error();

	// Extract Cb/Cr.
	GLuint fbo = resource_pool->create_fbo(dst_tex);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, WIDTH/2, HEIGHT/2);
	check_error();

	glUseProgram(cbcr_program_num);
	check_error();

	glActiveTexture(GL_TEXTURE0);
	check_error();
	glBindTexture(GL_TEXTURE_2D, src_tex);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	check_error();

	float chroma_offset_0[] = { -0.5f / WIDTH, 0.0f };
	set_uniform_vec2(cbcr_program_num, "foo", "chroma_offset_0", chroma_offset_0);

	GLuint position_vbo = fill_vertex_attribute(cbcr_program_num, "position", 2, GL_FLOAT, sizeof(vertices), vertices);
	GLuint texcoord_vbo = fill_vertex_attribute(cbcr_program_num, "texcoord", 2, GL_FLOAT, sizeof(vertices), vertices);  // Same as vertices.

	glDrawArrays(GL_TRIANGLES, 0, 3);
	check_error();

	cleanup_vertex_attribute(cbcr_program_num, "position", position_vbo);
	cleanup_vertex_attribute(cbcr_program_num, "texcoord", texcoord_vbo);

	glUseProgram(0);
	check_error();

	resource_pool->release_fbo(fbo);
	glDeleteVertexArrays(1, &vao);
}

void Mixer::release_display_frame(DisplayFrame *frame)
{
	for (GLuint texnum : frame->temp_textures) {
		resource_pool->release_2d_texture(texnum);
	}
	frame->temp_textures.clear();
	frame->ready_fence.reset();
	frame->input_frames.clear();
}

void Mixer::start()
{
	mixer_thread = thread(&Mixer::thread_func, this);
}

void Mixer::quit()
{
	should_quit = true;
	mixer_thread.join();
}

void Mixer::transition_clicked(int transition_num)
{
	theme->transition_clicked(transition_num, pts());
}

void Mixer::channel_clicked(int preview_num)
{
	theme->channel_clicked(preview_num);
}

Mixer::OutputChannel::~OutputChannel()
{
	if (has_current_frame) {
		parent->release_display_frame(&current_frame);
	}
	if (has_ready_frame) {
		parent->release_display_frame(&ready_frame);
	}
}

void Mixer::OutputChannel::output_frame(DisplayFrame frame)
{
	// Store this frame for display. Remove the ready frame if any
	// (it was seemingly never used).
	{
		unique_lock<mutex> lock(frame_mutex);
		if (has_ready_frame) {
			parent->release_display_frame(&ready_frame);
		}
		ready_frame = frame;
		has_ready_frame = true;
	}

	if (has_new_frame_ready_callback) {
		new_frame_ready_callback();
	}
}

bool Mixer::OutputChannel::get_display_frame(DisplayFrame *frame)
{
	unique_lock<mutex> lock(frame_mutex);
	if (!has_current_frame && !has_ready_frame) {
		return false;
	}

	if (has_current_frame && has_ready_frame) {
		// We have a new ready frame. Toss the current one.
		parent->release_display_frame(&current_frame);
		has_current_frame = false;
	}
	if (has_ready_frame) {
		assert(!has_current_frame);
		current_frame = ready_frame;
		ready_frame.ready_fence.reset();  // Drop the refcount.
		ready_frame.input_frames.clear();  // Drop the refcounts.
		has_current_frame = true;
		has_ready_frame = false;
	}

	*frame = current_frame;
	return true;
}

void Mixer::OutputChannel::set_frame_ready_callback(Mixer::new_frame_ready_callback_t callback)
{
	new_frame_ready_callback = callback;
	has_new_frame_ready_callback = true;
}
