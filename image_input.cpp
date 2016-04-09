#include "image_input.h"

#include <movit/image_format.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mutex>
#include <thread>

using namespace std;

ImageInput::ImageInput(const string &filename)
	: movit::FlatInput({movit::COLORSPACE_sRGB, movit::GAMMA_sRGB}, movit::FORMAT_RGBA_POSTMULTIPLIED_ALPHA,
	                   GL_UNSIGNED_BYTE, 1280, 720),  // FIXME
	  filename(filename),
	  current_image(load_image(filename))
{
	if (current_image == nullptr) {
		fprintf(stderr, "Couldn't load image, exiting.\n");
		exit(1);
	}
	set_pixel_data(current_image->pixels.get());
}

void ImageInput::set_gl_state(GLuint glsl_program_num, const string& prefix, unsigned *sampler_num)
{
	// See if the background thread has given us a new version of our image.
	// Note: The old version might still be lying around in other ImageInputs
	// (in fact, it's likely), but at least the total amount of memory used
	// is bounded. Currently we don't even share textures between them,
	// so there's a fair amount of OpenGL memory waste anyway (the cache
	// is mostly there to save startup time, not RAM).
	{
		unique_lock<mutex> lock(all_images_lock);
		if (all_images[filename] != current_image) {
			current_image = all_images[filename];
			set_pixel_data(current_image->pixels.get());
		}
	}
	movit::FlatInput::set_gl_state(glsl_program_num, prefix, sampler_num);
}

shared_ptr<const ImageInput::Image> ImageInput::load_image(const string &filename)
{
	unique_lock<mutex> lock(all_images_lock);  // Held also during loading.
	if (all_images.count(filename)) {
		return all_images[filename];
	}

	all_images[filename] = load_image_raw(filename);
	timespec first_modified = all_images[filename]->last_modified;
	update_threads[filename] =
		thread(bind(update_thread_func, filename, first_modified));

	return all_images[filename];
}

// Some helpers to make RAII versions of FFmpeg objects.
// The cleanup functions don't interact all that well with unique_ptr,
// so things get a bit messy and verbose, but overall it's worth it to ensure
// we never leak things by accident in error paths.

namespace {

void avformat_close_input_unique(AVFormatContext *format_ctx)
{
	avformat_close_input(&format_ctx);
}

unique_ptr<AVFormatContext, decltype(avformat_close_input_unique)*>
avformat_open_input_unique(const char *filename,
                           AVInputFormat *fmt, AVDictionary **options)
{
	AVFormatContext *format_ctx = nullptr;
	if (avformat_open_input(&format_ctx, filename, fmt, options) != 0) {
		format_ctx = nullptr;
	}
	return unique_ptr<AVFormatContext, decltype(avformat_close_input_unique)*>(
		format_ctx, avformat_close_input_unique);
}

void av_frame_free_unique(AVFrame *frame)
{
	av_frame_free(&frame);
}

unique_ptr<AVFrame, decltype(av_frame_free_unique)*>
av_frame_alloc_unique()
{
	AVFrame *frame = av_frame_alloc();
	return unique_ptr<AVFrame, decltype(av_frame_free_unique)*>(
		frame, av_frame_free_unique);
}

}  // namespace

shared_ptr<const ImageInput::Image> ImageInput::load_image_raw(const string &filename)
{
	// Note: Call before open, not after; otherwise, there's a race.
	// (There is now, too, but it tips the correct way. We could use fstat()
	// if we had the file descriptor.)
	struct stat buf;
	if (stat(filename.c_str(), &buf) != 0) {
		fprintf(stderr, "%s: Error stat-ing file\n", filename.c_str());
		return nullptr;
	}
	timespec last_modified = buf.st_mtim;

	auto format_ctx = avformat_open_input_unique(filename.c_str(), nullptr, nullptr);
	if (format_ctx == nullptr) {
		fprintf(stderr, "%s: Error opening file\n", filename.c_str());
		return nullptr;
	}

	if (avformat_find_stream_info(format_ctx.get(), nullptr) < 0) {
		fprintf(stderr, "%s: Error finding stream info\n", filename.c_str());
		return nullptr;
	}

	int stream_index = -1;
	for (unsigned i = 0; i < format_ctx->nb_streams; ++i) {
		if (format_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
			stream_index = i;
			break;
		}
	}
	if (stream_index == -1) {
		fprintf(stderr, "%s: No video stream found\n", filename.c_str());
		return nullptr;
	}

	AVCodecContext *codec_ctx = format_ctx->streams[stream_index]->codec;
	AVCodec *codec = avcodec_find_decoder(codec_ctx->codec_id);
	if (codec == nullptr) {
		fprintf(stderr, "%s: Cannot find decoder\n", filename.c_str());
		return nullptr;
	}
	if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
		fprintf(stderr, "%s: Cannot open decoder\n", filename.c_str());
		return nullptr;
	}
	unique_ptr<AVCodecContext, decltype(avcodec_close)*> codec_ctx_cleanup(
		codec_ctx, avcodec_close);

	// Read packets until we have a frame or there are none left.
	int frame_finished = 0;
	auto frame = av_frame_alloc_unique();
	do {
		AVPacket pkt;
		unique_ptr<AVPacket, decltype(av_packet_unref)*> pkt_cleanup(
			&pkt, av_packet_unref);
		av_init_packet(&pkt);
		pkt.data = nullptr;
		pkt.size = 0;
		if (av_read_frame(format_ctx.get(), &pkt) < 0) {
			break;
		}
		if (pkt.stream_index != stream_index) {
			continue;
		}

		if (avcodec_decode_video2(codec_ctx, frame.get(), &frame_finished, &pkt) < 0) {
			fprintf(stderr, "%s: Cannot decode frame\n", filename.c_str());
			return nullptr;
		}
	} while (!frame_finished);

	// See if there's a cached frame for us.
	if (!frame_finished) {
		AVPacket pkt;
		pkt.data = nullptr;
		pkt.size = 0;
		if (avcodec_decode_video2(codec_ctx, frame.get(), &frame_finished, &pkt) < 0) {
			fprintf(stderr, "%s: Cannot decode frame\n", filename.c_str());
			return nullptr;
		}
	}
	if (!frame_finished) {
		fprintf(stderr, "%s: Decoder did not output frame.\n", filename.c_str());
		return nullptr;
	}

	// TODO: Scale down if needed!
	uint8_t *pic_data[4] = {nullptr};
	unique_ptr<uint8_t *, decltype(av_freep)*> pic_data_cleanup(
		&pic_data[0], av_freep);
	int linesizes[4];
	if (av_image_alloc(pic_data, linesizes, frame->width, frame->height, AV_PIX_FMT_RGBA, 1) < 0) {
		fprintf(stderr, "%s: Could not allocate picture data\n", filename.c_str());
		return nullptr;
	}
	unique_ptr<SwsContext, decltype(sws_freeContext)*> sws_ctx(
		sws_getContext(frame->width, frame->height,
			(AVPixelFormat)frame->format, frame->width, frame->height,
			AV_PIX_FMT_RGBA, SWS_BICUBIC, nullptr, nullptr, nullptr),
		sws_freeContext);
	if (sws_ctx == nullptr) {
		fprintf(stderr, "%s: Could not create scaler context\n", filename.c_str());
		return nullptr;
	}
	sws_scale(sws_ctx.get(), frame->data, frame->linesize, 0, frame->height, pic_data, linesizes);

	size_t len = frame->width * frame->height * 4;
	unique_ptr<uint8_t[]> image_data(new uint8_t[len]);
	av_image_copy_to_buffer(image_data.get(), len, pic_data, linesizes, AV_PIX_FMT_RGBA, frame->width, frame->height, 1);

	shared_ptr<Image> image(new Image{move(image_data), last_modified});
	return image;
}

// Fire up a thread to update the image every second.
// We could do inotify, but this is good enough for now.
// TODO: These don't really quit, ever. Should they?
void ImageInput::update_thread_func(const std::string &filename, const timespec &first_modified)
{
	timespec last_modified = first_modified;
	struct stat buf;
	for ( ;; ) {
		sleep(1);

		if (stat(filename.c_str(), &buf) != 0) {
			fprintf(stderr, "%s: Couldn't check for new version, leaving the old in place.\n", filename.c_str());
			continue;
		}
		if (buf.st_mtim.tv_sec == last_modified.tv_sec &&
		    buf.st_mtim.tv_nsec == last_modified.tv_nsec) {
			// Not changed.
			continue;
		}
		shared_ptr<const Image> image = load_image_raw(filename);
		if (image == nullptr) {
			fprintf(stderr, "Couldn't load image, leaving the old in place.\n");
			continue;
		}
		fprintf(stderr, "Loaded new version of %s from disk.\n", filename.c_str());
		unique_lock<mutex> lock(all_images_lock);
		all_images[filename] = image;
		last_modified = image->last_modified;
	}
}

mutex ImageInput::all_images_lock;
map<string, shared_ptr<const ImageInput::Image>> ImageInput::all_images;
map<string, thread> ImageInput::update_threads;
