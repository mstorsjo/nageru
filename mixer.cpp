#define GL_GLEXT_PROTOTYPES 1
#define NO_SDL_GLEXT 1
#define NUM_CARDS 2

#define WIDTH 1280
#define HEIGHT 720

#include <epoxy/gl.h>
#include <epoxy/egl.h>

#undef Success

#include <assert.h>
#include <features.h>
#include <math.h>
#include <png.h>
#include <pngconf.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <mutex>
#include <queue>
#include <condition_variable>

#include <diffusion_effect.h>
#include <effect.h>
#include <effect_chain.h>
#include <flat_input.h>
#include <image_format.h>
#include <init.h>
#include <lift_gamma_gain_effect.h>
#include <saturation_effect.h>
#include <util.h>
#include <ycbcr_input.h>
#include <vignette_effect.h>
#include <resample_effect.h>
#include <resize_effect.h>
#include <overlay_effect.h>
#include <padding_effect.h>
#include <white_balance_effect.h>
#include <ycbcr.h>
#include <resource_pool.h>
#include <effect_util.h>

#include <EGL/egl.h>

#include "h264encode.h"
#include "context.h"
#include "bmusb.h"
#include "pbo_frame_allocator.h"

using namespace movit;
using namespace std;
using namespace std::placeholders;

// shared between all EGL contexts
EGLDisplay egl_display;
EGLSurface egl_surface;
EGLConfig ecfg;
EGLint ctxattr[] = {
	EGL_CONTEXT_CLIENT_VERSION, 2,
	EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
	EGL_CONTEXT_MINOR_VERSION_KHR, 1,
	//EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
	EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
	EGL_NONE
};

EGLConfig pbuffer_ecfg;

std::mutex bmusb_mutex;  // protects <cards>

struct CaptureCard {
	BMUSBCapture *usb;

	// Threading stuff
	bool thread_initialized;
	QSurface *surface;
	QOpenGLContext *context;

	bool new_data_ready;  // Whether new_frame contains anything.
	PBOFrameAllocator::Frame new_frame;
	GLsync new_data_ready_fence;  // Whether new_frame is ready for rendering.
	std::condition_variable new_data_ready_changed;  // Set whenever new_data_ready is changed.
};
CaptureCard cards[NUM_CARDS];

void bm_frame(int card_index, uint16_t timecode,
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
	GLuint pbo = (GLint)(intptr_t)video_frame.userdata;
	check_error();
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER_ARB, pbo);
	check_error();
	glFlushMappedBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, video_frame.size);
	check_error();
	//glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
	//check_error();
	GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, /*flags=*/0);              
	check_error();
	assert(fence != nullptr);
	{
		std::unique_lock<std::mutex> lock(bmusb_mutex);
		card->new_data_ready = true;
		card->new_frame = video_frame;
		card->new_data_ready_fence = fence;
		card->new_data_ready_changed.notify_all();
	}

	// Video frame will be released later.
        card->usb->get_audio_frame_allocator()->release_frame(audio_frame);
}
	
void place_rectangle(Effect *resample_effect, Effect *padding_effect, float x0, float y0, float x1, float y1)
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
	
static bool quit = false;
	
void mixer_thread_func(QSurface *surface, QSurface *surface2, QSurface *surface3, QSurface *surface4)
{
	cards[0].surface = surface3;
#if NUM_CARDS == 2
	cards[1].surface = surface4;
#endif

	eglBindAPI(EGL_OPENGL_API);
	//QSurface *surface = create_surface();
	QOpenGLContext *context = create_context();
	if (!make_current(context, surface)) {
		printf("oops\n");
		exit(1);
	}
	printf("egl=%p\n", eglGetCurrentContext());

	CHECK(init_movit("/usr/share/movit", MOVIT_DEBUG_ON));
	printf("GPU texture subpixel precision: about %.1f bits\n",
		log2(1.0f / movit_texel_subpixel_precision));
	printf("Wrongly rounded x+0.48 or x+0.52 values: %d/510\n",
		movit_num_wrongly_rounded);
	if (movit_num_wrongly_rounded > 0) {
		if (movit_shader_rounding_supported) {
			printf("Rounding off in the shader to compensate.\n");
		} else {
			printf("No shader roundoff available; cannot compensate.\n");
		}
	}

	//printf("egl_api=%d EGL_OPENGL_API=%d\n", epoxy_egl_get_current_gl_context_api(), EGL_OPENGL_API);
	//exit(0);

	check_error();

	EffectChain chain(WIDTH, HEIGHT);
	check_error();
#if 0
	glViewport(0, 0, WIDTH, HEIGHT);
	check_error();

	glMatrixMode(GL_PROJECTION);
	check_error();
	glLoadIdentity();
	check_error();
	glOrtho(0.0, 1.0, 0.0, 1.0, 0.0, 1.0);
	check_error();

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
#endif

	ImageFormat inout_format;
	inout_format.color_space = COLORSPACE_sRGB;
	inout_format.gamma_curve = GAMMA_sRGB;

	YCbCrFormat ycbcr_format;
	ycbcr_format.chroma_subsampling_x = 2;
	ycbcr_format.chroma_subsampling_y = 1;
	ycbcr_format.cb_x_position = 0.0;
	ycbcr_format.cr_x_position = 0.0;
	ycbcr_format.cb_y_position = 0.5;
	ycbcr_format.cr_y_position = 0.5;
	ycbcr_format.luma_coefficients = YCBCR_REC_601;
	ycbcr_format.full_range = false;

	YCbCrInput *input[NUM_CARDS];

	input[0] = new YCbCrInput(inout_format, ycbcr_format, WIDTH, HEIGHT, YCBCR_INPUT_SPLIT_Y_AND_CBCR);
	chain.add_input(input[0]);
	//if (NUM_CARDS == 2) {
		input[1] = new YCbCrInput(inout_format, ycbcr_format, WIDTH, HEIGHT, YCBCR_INPUT_SPLIT_Y_AND_CBCR);
		chain.add_input(input[1]);
	//}
	//YCbCr422InterleavedInput *input = new YCbCr422InterleavedInput(inout_format, ycbcr_format, WIDTH, HEIGHT);
	//YCbCr422InterleavedInput *input = new YCbCr422InterleavedInput(inout_format, ycbcr_format, 2, 1);
	Effect *resample_effect = chain.add_effect(new ResampleEffect(), input[0]);
	Effect *padding_effect = chain.add_effect(new IntegralPaddingEffect());
	float border_color[] = { 0.0f, 0.0f, 0.0f, 1.0f };
	CHECK(padding_effect->set_vec4("border_color", border_color));

	//Effect *resample2_effect = chain.add_effect(new ResampleEffect(), input[1 % NUM_CARDS]);
	Effect *resample2_effect = chain.add_effect(new ResampleEffect(), input[1]);
	Effect *saturation_effect = chain.add_effect(new SaturationEffect());
	CHECK(saturation_effect->set_float("saturation", 0.3f));
	Effect *wb_effect = chain.add_effect(new WhiteBalanceEffect());
	CHECK(wb_effect->set_float("output_color_temperature", 3500.0));
	Effect *padding2_effect = chain.add_effect(new IntegralPaddingEffect());

	chain.add_effect(new OverlayEffect(), padding_effect, padding2_effect);

	ycbcr_format.chroma_subsampling_x = 1;

	chain.add_ycbcr_output(inout_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED, ycbcr_format, YCBCR_OUTPUT_SPLIT_Y_AND_CBCR);
	chain.set_dither_bits(8);
	chain.set_output_origin(OUTPUT_ORIGIN_TOP_LEFT);
	chain.finalize();

#if 0
	// generate a PBO to hold the data we read back with glReadPixels()
	// (Intel/DRI goes into a slow path if we don't read to PBO)
	GLuint pbo;
	glGenBuffers(1, &pbo);
	glBindBuffer(GL_PIXEL_PACK_BUFFER_ARB, pbo);
	glBufferData(GL_PIXEL_PACK_BUFFER_ARB, WIDTH * HEIGHT * 4, NULL, GL_STREAM_READ);
#endif

	//make_hsv_wheel_texture();

//	QSurface *create_surface(const QSurfaceFormat &format);
	H264Encoder h264_encoder(surface2, WIDTH, HEIGHT, "test.mp4");

	printf("Configuring first card...\n");
	cards[0].usb = new BMUSBCapture(0x1edb, 0xbd3b);  // 0xbd4f
	//cards[0].usb = new BMUSBCapture(0x1edb, 0xbd4f);
	cards[0].usb->set_frame_callback(std::bind(bm_frame, 0, _1, _2, _3, _4, _5, _6, _7));
	std::unique_ptr<PBOFrameAllocator> pbo_allocator1(new PBOFrameAllocator(1280 * 750 * 2 + 44));
	cards[0].usb->set_video_frame_allocator(pbo_allocator1.get());
	cards[0].usb->configure_card();

	std::unique_ptr<PBOFrameAllocator> pbo_allocator2(new PBOFrameAllocator(1280 * 750 * 2 + 44));
	if (NUM_CARDS == 2) {
		printf("Configuring second card...\n");
		cards[1].usb = new BMUSBCapture(0x1edb, 0xbd4f);
		cards[1].usb->set_frame_callback(std::bind(bm_frame, 1, _1, _2, _3, _4, _5, _6, _7));
		cards[1].usb->set_video_frame_allocator(pbo_allocator2.get());
		cards[1].usb->configure_card();
	}

	BMUSBCapture::start_bm_thread();

	for (int card_index = 0; card_index < NUM_CARDS; ++card_index) {
		cards[card_index].usb->start_bm_capture();
	}

	int frame = 0;
#if _POSIX_C_SOURCE >= 199309L
	struct timespec start, now;
	clock_gettime(CLOCK_MONOTONIC, &start);
#else
	struct timeval start, now;
	gettimeofday(&start, NULL);
#endif

	PBOFrameAllocator::Frame bmusb_current_rendering_frame[NUM_CARDS];
	for (int card_index = 0; card_index < NUM_CARDS; ++card_index) {
		bmusb_current_rendering_frame[card_index] =
			cards[card_index].usb->get_video_frame_allocator()->alloc_frame();
		GLint input_tex_pbo = (GLint)(intptr_t)bmusb_current_rendering_frame[card_index].userdata;
		input[card_index]->set_pixel_data(0, nullptr, input_tex_pbo);
		input[card_index]->set_pixel_data(1, nullptr, input_tex_pbo);
		check_error();
	}

	//chain.enable_phase_timing(true);

	// Set up stuff for NV12 conversion.
#if 0
	PBOFrameAllocator nv12_frame_pool(WIDTH * HEIGHT * 3 / 2, /*num_frames=*/24,
		GL_PIXEL_PACK_BUFFER_ARB, GL_MAP_READ_BIT | GL_MAP_COHERENT_BIT, 0);
#endif
	ResourcePool *resource_pool = chain.get_resource_pool();
	//GLuint ycbcr_tex = resource_pool->create_2d_texture(GL_RGBA8, WIDTH, HEIGHT);
	GLuint chroma_tex = resource_pool->create_2d_texture(GL_RG8, WIDTH, HEIGHT);

#if 0
	// Y shader.
	string y_vert_shader = read_version_dependent_file("vs-y", "vert");
	string y_frag_shader =
		"#version 130 \n"
		"in vec2 tc; \n"
		"uniform sampler2D ycbcr_tex; \n"
		"void main() { \n"
		"    gl_FragColor = texture2D(ycbcr_tex, tc); \n"
		"} \n";
	GLuint y_program_num = resource_pool->compile_glsl_program(y_vert_shader, y_frag_shader);
#endif

#if 1
	// Cb/Cr shader.
	string cbcr_vert_shader = read_file("vs-cbcr.130.vert");
	string cbcr_frag_shader =
		"#version 130 \n"
		"in vec2 tc0; \n"
//		"in vec2 tc1; \n"
		"uniform sampler2D cbcr_tex; \n"
		"void main() { \n"
		"    gl_FragColor = texture2D(cbcr_tex, tc0); \n"
//		"    gl_FragColor.ba = texture2D(cbcr_tex, tc1).gb; \n"
		"} \n";
	GLuint cbcr_program_num = resource_pool->compile_glsl_program(cbcr_vert_shader, cbcr_frag_shader);
#endif

	GLuint vao;
	glGenVertexArrays(1, &vao);
	check_error();

	while (!quit) {
		++frame;

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
		
		float t = 0.5 + 0.5 * cos(frame * 0.006);
		//float t = 0.0;
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

		place_rectangle(resample_effect, padding_effect, left0, top0, right0, bottom0);
		place_rectangle(resample2_effect, padding2_effect, left1, top1, right1, bottom1);

		CaptureCard card_copy[NUM_CARDS];

		{
			std::unique_lock<std::mutex> lock(bmusb_mutex);

			// The first card is the master timer, so wait for it to have a new frame.
			// TODO: Make configurable, and with a timeout.
			cards[0].new_data_ready_changed.wait(lock, []{ return cards[0].new_data_ready; });

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

			// FIXME: We could still be rendering from it! 
			card->usb->get_video_frame_allocator()->release_frame(bmusb_current_rendering_frame[card_index]);
			bmusb_current_rendering_frame[card_index] = card->new_frame;

			// The new texture might still be uploaded,
			// tell the GPU to wait until it's there.
			if (card->new_data_ready_fence)
				glWaitSync(card->new_data_ready_fence, /*flags=*/0, GL_TIMEOUT_IGNORED);
			check_error();
			glDeleteSync(card->new_data_ready_fence);
			check_error();
			GLint input_tex_pbo = (GLint)(intptr_t)bmusb_current_rendering_frame[card_index].userdata;
			input[card_index]->set_pixel_data(0, (unsigned char *)BUFFER_OFFSET((1280 * 750 * 2 + 44) / 2 + 1280 * 25 + 22), input_tex_pbo);
			input[card_index]->set_pixel_data(1, (unsigned char *)BUFFER_OFFSET(1280 * 25 + 22), input_tex_pbo);

			if (NUM_CARDS == 1) {
				// Set to the other one, too.
				input[1]->set_pixel_data(0, (unsigned char *)BUFFER_OFFSET((1280 * 750 * 2 + 44) / 2 + 1280 * 25 + 22), input_tex_pbo);
				input[1]->set_pixel_data(1, (unsigned char *)BUFFER_OFFSET(1280 * 25 + 22), input_tex_pbo);
			}
		}

		GLuint y_tex, cbcr_tex;
		bool got_frame = h264_encoder.begin_frame(&y_tex, &cbcr_tex);
		assert(got_frame);

		// Render chain.
		{
			GLuint ycbcr_fbo = resource_pool->create_fbo(y_tex, chroma_tex);
			chain.render_to_fbo(ycbcr_fbo, WIDTH, HEIGHT);
			resource_pool->release_fbo(ycbcr_fbo);
		}
		if (false) {
			glViewport(0, 0, WIDTH, HEIGHT);
			chain.render_to_screen();
		}

#if 0
		PBOFrameAllocator::Frame nv12_frame = nv12_frame_pool.alloc_frame();
		assert(nv12_frame.data != nullptr);  // should never happen... maybe?
		GLuint pbo = (GLuint)(intptr_t)nv12_frame.userdata;
		glBindBuffer(GL_PIXEL_PACK_BUFFER_ARB, pbo);
		check_error();
#endif

		// Set up for extraction.
		float vertices[] = {
			0.0f, 2.0f,
			0.0f, 0.0f,
			2.0f, 0.0f
		};

		glBindVertexArray(vao);
		check_error();

#if 0
		// Extract Y.
		GLuint y_fbo = resource_pool->create_fbo(y_tex);
		glBindFramebuffer(GL_FRAMEBUFFER, y_fbo);
		glViewport(0, 0, WIDTH, HEIGHT);
		check_error();
		{
			glUseProgram(y_program_num);
			check_error();
		        glActiveTexture(GL_TEXTURE0);
			check_error();
			glBindTexture(GL_TEXTURE_2D, ycbcr_tex);
			check_error();
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			check_error();
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			check_error();
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			check_error();

			GLuint position_vbo = fill_vertex_attribute(y_program_num, "position", 2, GL_FLOAT, sizeof(vertices), vertices);
			GLuint texcoord_vbo = fill_vertex_attribute(y_program_num, "texcoord", 2, GL_FLOAT, sizeof(vertices), vertices);  // Same as vertices.

			glDrawArrays(GL_TRIANGLES, 0, 3);
			check_error();

			cleanup_vertex_attribute(y_program_num, "position", position_vbo);
			check_error();
			cleanup_vertex_attribute(y_program_num, "texcoord", texcoord_vbo);
			check_error();

			resource_pool->release_fbo(y_fbo);
		}
#endif

		// Extract Cb/Cr.
		GLuint cbcr_fbo = resource_pool->create_fbo(cbcr_tex);
		glBindFramebuffer(GL_FRAMEBUFFER, cbcr_fbo);
		glViewport(0, 0, WIDTH/2, HEIGHT/2);
		check_error();
		GLsync fence;
		{
			glUseProgram(cbcr_program_num);
			check_error();

		        glActiveTexture(GL_TEXTURE0);
			check_error();
			glBindTexture(GL_TEXTURE_2D, chroma_tex);
			check_error();
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			check_error();
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			check_error();
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			check_error();

			float chroma_offset_0[] = { -0.5f / WIDTH, 0.0f };
//			float chroma_offset_1[] = { +0.5f / WIDTH, 0.0f };
			set_uniform_vec2(cbcr_program_num, "foo", "chroma_offset_0", chroma_offset_0);
//			set_uniform_vec2(cbcr_program_num, "foo", "chroma_offset_1", chroma_offset_1);

			GLuint position_vbo = fill_vertex_attribute(cbcr_program_num, "position", 2, GL_FLOAT, sizeof(vertices), vertices);
			GLuint texcoord_vbo = fill_vertex_attribute(cbcr_program_num, "texcoord", 2, GL_FLOAT, sizeof(vertices), vertices);  // Same as vertices.

			glDrawArrays(GL_TRIANGLES, 0, 3);
			check_error();

			cleanup_vertex_attribute(cbcr_program_num, "position", position_vbo);
			cleanup_vertex_attribute(cbcr_program_num, "texcoord", texcoord_vbo);

			glUseProgram(0);
			check_error();

#if 0	
			glReadPixels(0, 0, WIDTH/4, HEIGHT/2, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, BUFFER_OFFSET(WIDTH * HEIGHT));
			check_error();
#endif

			fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, /*flags=*/0);              
			check_error();

			resource_pool->release_fbo(cbcr_fbo);
		}

#if 0
		glBindBuffer(GL_PIXEL_PACK_BUFFER_ARB, 0);
#endif

#if 1
		h264_encoder.end_frame(fence);
#else
		nv12_frame_pool.release_frame(nv12_frame);
#endif

		//eglSwapBuffers(egl_display, egl_surface);


#if 1
#if _POSIX_C_SOURCE >= 199309L
		clock_gettime(CLOCK_MONOTONIC, &now);
		double elapsed = now.tv_sec - start.tv_sec +
			1e-9 * (now.tv_nsec - start.tv_nsec);
#else
		gettimeofday(&now, NULL);
		double elapsed = now.tv_sec - start.tv_sec +
			1e-6 * (now.tv_usec - start.tv_usec);
#endif
		if (frame % 100 == 0) {
			printf("%d frames in %.3f seconds = %.1f fps (%.1f ms/frame)\n",
				frame, elapsed, frame / elapsed,
				1e3 * elapsed / frame);
		//	chain.print_phase_timing();
		}

		// Reset every 100 frames, so that local variations in frame times
		// (especially for the first few frames, when the shaders are
		// compiled etc.) don't make it hard to measure for the entire
		// remaining duration of the program.
		if (frame == 10000) {
			frame = 0;
			start = now;
		}
#endif
	}
	glDeleteVertexArrays(1, &vao);
	//resource_pool->release_glsl_program(y_program_num);
	resource_pool->release_glsl_program(cbcr_program_num);
	resource_pool->release_2d_texture(chroma_tex);
	BMUSBCapture::stop_bm_thread();
}

std::thread mixer_thread;

void start_mixer(QSurface *surface, QSurface *surface2, QSurface *surface3, QSurface *surface4)
{
	mixer_thread = std::thread([surface, surface2, surface3, surface4]{
		mixer_thread_func(surface, surface2, surface3, surface4);
	});
}

void mixer_quit()
{
	quit = true;
	mixer_thread.join();
}
