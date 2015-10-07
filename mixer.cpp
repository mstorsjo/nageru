#define WIDTH 1280
#define HEIGHT 720

#undef Success

#include "mixer.h"

#include <assert.h>
#include <effect.h>
#include <effect_chain.h>
#include <effect_util.h>
#include <epoxy/egl.h>
#include <features.h>
#include <image_format.h>
#include <init.h>
#include <overlay_effect.h>
#include <padding_effect.h>
#include <resample_effect.h>
#include <resource_pool.h>
#include <saturation_effect.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <util.h>
#include <white_balance_effect.h>
#include <ycbcr.h>
#include <ycbcr_input.h>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bmusb.h"
#include "context.h"
#include "h264encode.h"
#include "pbo_frame_allocator.h"
#include "ref_counted_gl_sync.h"

class QOpenGLContext;

using namespace movit;
using namespace std;
using namespace std::placeholders;

Mixer *global_mixer = nullptr;

Mixer::Mixer(const QSurfaceFormat &format)
	: mixer_surface(create_surface(format)),
	  h264_encoder_surface(create_surface(format))
{
	CHECK(init_movit(MOVIT_SHADER_DIR, MOVIT_DEBUG_OFF));
	check_error();

	resource_pool.reset(new ResourcePool);
	theme.reset(new Theme("theme.lua", resource_pool.get()));
	output_channel[OUTPUT_LIVE].parent = this;
	output_channel[OUTPUT_PREVIEW].parent = this;
	output_channel[OUTPUT_INPUT0].parent = this;
	output_channel[OUTPUT_INPUT1].parent = this;

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

	h264_encoder.reset(new H264Encoder(h264_encoder_surface, WIDTH, HEIGHT, "test.mp4"));

	printf("Configuring first card...\n");
	cards[0].usb = new BMUSBCapture(0x1edb, 0xbd3b);  // 0xbd4f
	cards[0].usb->set_frame_callback(std::bind(&Mixer::bm_frame, this, 0, _1, _2, _3, _4, _5, _6, _7));
	cards[0].frame_allocator.reset(new PBOFrameAllocator(1280 * 750 * 2 + 44, 1280, 720));
	cards[0].usb->set_video_frame_allocator(cards[0].frame_allocator.get());
	cards[0].usb->configure_card();
	cards[0].surface = create_surface(format);
#if NUM_CARDS == 2
	cards[1].surface = create_surface(format);
#endif

	if (NUM_CARDS == 2) {
		printf("Configuring second card...\n");
		cards[1].usb = new BMUSBCapture(0x1edb, 0xbd4f);
		cards[1].usb->set_frame_callback(std::bind(&Mixer::bm_frame, this, 1, _1, _2, _3, _4, _5, _6, _7));
		cards[1].frame_allocator.reset(new PBOFrameAllocator(1280 * 750 * 2 + 44, 1280, 720));
		cards[1].usb->set_video_frame_allocator(cards[1].frame_allocator.get());
		cards[1].usb->configure_card();
	}

	BMUSBCapture::start_bm_thread();

	for (int card_index = 0; card_index < NUM_CARDS; ++card_index) {
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
}

Mixer::~Mixer()
{
	resource_pool->release_glsl_program(cbcr_program_num);
	BMUSBCapture::stop_bm_thread();
}

void Mixer::bm_frame(int card_index, uint16_t timecode,
                     FrameAllocator::Frame video_frame, size_t video_offset, uint16_t video_format,
		     FrameAllocator::Frame audio_frame, size_t audio_offset, uint16_t audio_format)
{
	CaptureCard *card = &cards[card_index];
	if (!card->thread_initialized) {
		printf("initializing context for bmusb thread %d\n", card_index);
		eglBindAPI(EGL_OPENGL_API);
		card->context = create_context();
		if (!make_current(card->context, card->surface)) {
			printf("failed to create bmusb context\n");
			exit(1);
		}
		card->thread_initialized = true;
	}	

	if (video_frame.len - video_offset != 1280 * 750 * 2) {
		printf("dropping frame with wrong length (%ld)\n", video_frame.len - video_offset);
		FILE *fp = fopen("frame.raw", "wb");
		fwrite(video_frame.data, video_frame.len, 1, fp);
		fclose(fp);
		//exit(1);
		card->usb->get_video_frame_allocator()->release_frame(video_frame);
		card->usb->get_audio_frame_allocator()->release_frame(audio_frame);
		return;
	}
	{
		// Wait until the previous frame was consumed.
		std::unique_lock<std::mutex> lock(bmusb_mutex);
		card->new_data_ready_changed.wait(lock, [card]{ return !card->new_data_ready; });
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
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1280, 720, GL_RED, GL_UNSIGNED_BYTE, BUFFER_OFFSET((1280 * 750 * 2 + 44) / 2 + 1280 * 25 + 22));
	check_error();
	glBindTexture(GL_TEXTURE_2D, userdata->tex_cbcr);
	check_error();
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1280/2, 720, GL_RG, GL_UNSIGNED_BYTE, BUFFER_OFFSET(1280 * 25 + 22));
	check_error();
	glBindTexture(GL_TEXTURE_2D, 0);
	check_error();
	GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, /*flags=*/0);              
	check_error();
	assert(fence != nullptr);
	{
		std::unique_lock<std::mutex> lock(bmusb_mutex);
		card->new_data_ready = true;
		card->new_frame = RefCountedFrame(video_frame);
		card->new_data_ready_fence = fence;
		card->new_data_ready_changed.notify_all();
	}

	// Video frame will be released when last user of card->new_frame goes out of scope.
        card->usb->get_audio_frame_allocator()->release_frame(audio_frame);
}
	
void Mixer::place_rectangle(Effect *resample_effect, Effect *padding_effect, float x0, float y0, float x1, float y1)
{
	float srcx0 = 0.0f;
	float srcx1 = 1.0f;
	float srcy0 = 0.0f;
	float srcy1 = 1.0f;

	// Cull.
	if (x0 > 1280.0 || x1 < 0.0 || y0 > 720.0 || y1 < 0.0) {
		CHECK(resample_effect->set_int("width", 1));
		CHECK(resample_effect->set_int("height", 1));
		CHECK(resample_effect->set_float("zoom_x", 1280.0));
		CHECK(resample_effect->set_float("zoom_y", 720.0));
		CHECK(padding_effect->set_int("left", 2000));
		CHECK(padding_effect->set_int("top", 2000));
		return;	
	}

	// Clip. (TODO: Clip on upper/left sides, too.)
	if (x1 > 1280.0) {
		srcx1 = (1280.0 - x0) / (x1 - x0);
		x1 = 1280.0;
	}
	if (y1 > 720.0) {
		srcy1 = (720.0 - y0) / (y1 - y0);
		y1 = 720.0;
	}

	float x_subpixel_offset = x0 - floor(x0);
	float y_subpixel_offset = y0 - floor(y0);

	// Resampling must be to an integral number of pixels. Round up,
	// and then add an extra pixel so we have some leeway for the border.
	int width = int(ceil(x1 - x0)) + 1;
	int height = int(ceil(y1 - y0)) + 1;
	CHECK(resample_effect->set_int("width", width));
	CHECK(resample_effect->set_int("height", height));

	// Correct the discrepancy with zoom. (This will leave a small
	// excess edge of pixels and subpixels, which we'll correct for soon.)
	float zoom_x = (x1 - x0) / (width * (srcx1 - srcx0));
	float zoom_y = (y1 - y0) / (height * (srcy1 - srcy0));
	CHECK(resample_effect->set_float("zoom_x", zoom_x));
	CHECK(resample_effect->set_float("zoom_y", zoom_y));
	CHECK(resample_effect->set_float("zoom_center_x", 0.0f));
	CHECK(resample_effect->set_float("zoom_center_y", 0.0f));

	// Padding must also be to a whole-pixel offset.
	CHECK(padding_effect->set_int("left", floor(x0)));
	CHECK(padding_effect->set_int("top", floor(y0)));

	// Correct _that_ discrepancy by subpixel offset in the resampling.
	CHECK(resample_effect->set_float("left", -x_subpixel_offset / zoom_x));
	CHECK(resample_effect->set_float("top", -y_subpixel_offset / zoom_y));

	// Finally, adjust the border so it is exactly where we want it.
	CHECK(padding_effect->set_float("border_offset_left", x_subpixel_offset));
	CHECK(padding_effect->set_float("border_offset_right", x1 - (floor(x0) + width)));
	CHECK(padding_effect->set_float("border_offset_top", y_subpixel_offset));
	CHECK(padding_effect->set_float("border_offset_bottom", y1 - (floor(y0) + height)));
}
	
void Mixer::thread_func()
{
	eglBindAPI(EGL_OPENGL_API);
	QOpenGLContext *context = create_context();
	if (!make_current(context, mixer_surface)) {
		printf("oops\n");
		exit(1);
	}

	struct timespec start, now;
	clock_gettime(CLOCK_MONOTONIC, &start);

	while (!should_quit) {
		++frame;

#if 0
		//int width0 = lrintf(848 * (1.0 + 0.2 * sin(frame * 0.02)));
		int width0 = 848;
		int height0 = lrintf(width0 * 9.0 / 16.0);

		//float top0 = 96 + 48 * sin(frame * 0.005);
		//float left0 = 96 + 48 * cos(frame * 0.006);
		float top0 = 48;
		float left0 = 16;
		float bottom0 = top0 + height0;
		float right0 = left0 + width0;

		int width1 = 384;
		int height1 = 216;
	
		float bottom1 = 720 - 48;
		float right1 = 1280 - 16;
		float top1 = bottom1 - height1;
		float left1 = right1 - width1;
	
		if (current_source == SOURCE_INPUT1) {
			top0 = 0.0;
			bottom0 = HEIGHT;
			left0 = 0.0;
			right0 = WIDTH;

			top1 = HEIGHT + 10;
			bottom1 = HEIGHT + 20;
			left1 = WIDTH + 10;
			right1 = WIDTH + 20;
		} else if (current_source == SOURCE_INPUT2) {
			top1 = 0.0;
			bottom1 = HEIGHT;
			left1 = 0.0;
			right1 = WIDTH;

			top0 = HEIGHT + 10;
			bottom0 = HEIGHT + 20;
			left0 = WIDTH + 10;
			right0 = WIDTH + 20;
		} else {
			float t = 0.5 + 0.5 * cos(frame * 0.006);
			float scale0 = 1.0 + t * (1280.0 / 848.0 - 1.0);
			float tx0 = 0.0 + t * (-16.0 * scale0);
			float ty0 = 0.0 + t * (-48.0 * scale0);

			top0 = top0 * scale0 + ty0;
			bottom0 = bottom0 * scale0 + ty0;
			left0 = left0 * scale0 + tx0;
			right0 = right0 * scale0 + tx0;

			top1 = top1 * scale0 + ty0;
			bottom1 = bottom1 * scale0 + ty0;
			left1 = left1 * scale0 + tx0;
			right1 = right1 * scale0 + tx0;
		}

		place_rectangle(resample_effect, padding_effect, left0, top0, right0, bottom0);
		place_rectangle(resample2_effect, padding2_effect, left1, top1, right1, bottom1);
#endif

		CaptureCard card_copy[NUM_CARDS];

		{
			std::unique_lock<std::mutex> lock(bmusb_mutex);

			// The first card is the master timer, so wait for it to have a new frame.
			// TODO: Make configurable, and with a timeout.
			cards[0].new_data_ready_changed.wait(lock, [this]{ return cards[0].new_data_ready; });

			for (int card_index = 0; card_index < NUM_CARDS; ++card_index) {
				CaptureCard *card = &cards[card_index];
				card_copy[card_index].usb = card->usb;
				card_copy[card_index].new_data_ready = card->new_data_ready;
				card_copy[card_index].new_frame = card->new_frame;
				card_copy[card_index].new_data_ready_fence = card->new_data_ready_fence;
				card->new_data_ready = false;
				card->new_data_ready_changed.notify_all();
			}
		}

		for (int card_index = 0; card_index < NUM_CARDS; ++card_index) {
			CaptureCard *card = &card_copy[card_index];
			if (!card->new_data_ready)
				continue;

			assert(card->new_frame != nullptr);
			bmusb_current_rendering_frame[card_index] = card->new_frame;
			check_error();

			// The new texture might still be uploaded,
			// tell the GPU to wait until it's there.
			if (card->new_data_ready_fence)
				glWaitSync(card->new_data_ready_fence, /*flags=*/0, GL_TIMEOUT_IGNORED);
			check_error();
			glDeleteSync(card->new_data_ready_fence);
			check_error();
			const PBOFrameAllocator::Userdata *userdata = (const PBOFrameAllocator::Userdata *)card->new_frame->userdata;
			theme->set_input_textures(card_index, userdata->tex_y, userdata->tex_cbcr);
		}

		// Get the main chain from the theme, and set its state immediately.
		pair<EffectChain *, function<void()>> theme_main_chain = theme->get_chain(0, frame / 60.0f, WIDTH, HEIGHT);
		EffectChain *chain = theme_main_chain.first;
		theme_main_chain.second();

		GLuint y_tex, cbcr_tex;
		bool got_frame = h264_encoder->begin_frame(&y_tex, &cbcr_tex);
		assert(got_frame);

		// Render main chain.
		GLuint cbcr_full_tex = resource_pool->create_2d_texture(GL_RG8, WIDTH, HEIGHT);
		GLuint rgba_tex = resource_pool->create_2d_texture(GL_RGB565, WIDTH, HEIGHT);  // Saves texture bandwidth, although dithering gets messed up.
		GLuint fbo = resource_pool->create_fbo(y_tex, cbcr_full_tex, rgba_tex);
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
		for (int card_index = 0; card_index < NUM_CARDS; ++card_index) {
			input_frames.push_back(bmusb_current_rendering_frame[card_index]);
		}
		h264_encoder->end_frame(fence, input_frames);

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
		for (unsigned i = 1; i < theme->get_num_channels() + 2; ++i) {
			DisplayFrame display_frame;
			pair<EffectChain *, function<void()>> chain = theme->get_chain(i, frame / 60.0f, WIDTH, HEIGHT);  // FIXME: dimensions
			display_frame.chain = chain.first;
			display_frame.setup_chain = chain.second;
			display_frame.ready_fence = fence;
			display_frame.input_frames = { bmusb_current_rendering_frame[0], bmusb_current_rendering_frame[1] };  // FIXME: possible to do better?
			display_frame.temp_textures = {};
			output_channel[i].output_frame(display_frame);
		}

		clock_gettime(CLOCK_MONOTONIC, &now);
		double elapsed = now.tv_sec - start.tv_sec +
			1e-9 * (now.tv_nsec - start.tv_nsec);
		if (frame % 100 == 0) {
			printf("%d frames in %.3f seconds = %.1f fps (%.1f ms/frame)\n",
				frame, elapsed, frame / elapsed,
				1e3 * elapsed / frame);
		//	chain->print_phase_timing();
		}

		// Reset every 100 frames, so that local variations in frame times
		// (especially for the first few frames, when the shaders are
		// compiled etc.) don't make it hard to measure for the entire
		// remaining duration of the program.
		if (frame == 10000) {
			frame = 0;
			start = now;
		}
		check_error();
	}
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
	mixer_thread = std::thread(&Mixer::thread_func, this);
}

void Mixer::quit()
{
	should_quit = true;
	mixer_thread.join();
}

void Mixer::transition_clicked(int transition_num, float t)
{
	theme->transition_clicked(transition_num, t);
}

void Mixer::OutputChannel::output_frame(DisplayFrame frame)
{
	// Store this frame for display. Remove the ready frame if any
	// (it was seemingly never used).
	{
		std::unique_lock<std::mutex> lock(frame_mutex);
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
	std::unique_lock<std::mutex> lock(frame_mutex);
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
