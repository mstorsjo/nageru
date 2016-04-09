#include "image_input.h"

#include <movit/image_format.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

using namespace std;

ImageInput::ImageInput(const string &filename)
	: movit::FlatInput({movit::COLORSPACE_sRGB, movit::GAMMA_sRGB}, movit::FORMAT_RGBA_POSTMULTIPLIED_ALPHA,
	                   GL_UNSIGNED_BYTE, 1280, 720)  // FIXME
{
	const uint8_t *pixel_data = load_image(filename);
	if (pixel_data == nullptr) {
		fprintf(stderr, "Couldn't load image, exiting.\n");
		exit(1);
	}
	set_pixel_data(pixel_data);
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

const uint8_t *ImageInput::load_image(const string &filename)
{
	if (all_images.count(filename)) {
		return all_images[filename].get();
	}

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

	// Read packets until we have a frame.
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
			fprintf(stderr, "%s: Cannot read frame\n", filename.c_str());
			return nullptr;
		}
		if (pkt.stream_index != stream_index) {
			continue;
		}

		if (avcodec_decode_video2(codec_ctx, frame.get(), &frame_finished, &pkt) < 0) {
			fprintf(stderr, "%s: Cannot decode frame\n", filename.c_str());
			return nullptr;
		}
	} while (!frame_finished);

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

	all_images[filename] = move(image_data);
	return all_images[filename].get();
}

map<string, unique_ptr<uint8_t[]>> ImageInput::all_images;
