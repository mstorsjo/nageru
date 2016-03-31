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
	set_pixel_data(load_image(filename));
}

const uint8_t *ImageInput::load_image(const string &filename)
{
	if (all_images.count(filename)) {
		return all_images[filename].get();
	}

	AVFormatContext *format_ctx = nullptr;
	if (avformat_open_input(&format_ctx, filename.c_str(), nullptr, nullptr) != 0) {
		fprintf(stderr, "%s: Error opening file\n", filename.c_str());
		exit(1);
	}

	if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
		fprintf(stderr, "%s: Error finding stream info\n", filename.c_str());
		exit(1);
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
		exit(1);
	}

	AVCodecContext *codec_ctx = format_ctx->streams[stream_index]->codec;
	AVCodec *codec = avcodec_find_decoder(codec_ctx->codec_id);
	if (codec == nullptr) {
		fprintf(stderr, "%s: Cannot find decoder\n", filename.c_str());
		exit(1);
	}
	if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
		fprintf(stderr, "%s: Cannot open decoder\n", filename.c_str());
		exit(1);
	}

	// Read packets until we have a frame.
	int frame_finished = 0;
	AVFrame *frame = av_frame_alloc();
	do {
		AVPacket pkt;
		av_init_packet(&pkt);
		pkt.data = nullptr;
		pkt.size = 0;
		if (av_read_frame(format_ctx, &pkt) < 0) {
			fprintf(stderr, "%s: Cannot read frame\n", filename.c_str());
			exit(1);
		}
		if (pkt.stream_index != stream_index) {
			av_free_packet(&pkt);
			continue;
		}

		if (avcodec_decode_video2(codec_ctx, frame, &frame_finished, &pkt) < 0) {
			fprintf(stderr, "%s: Cannot decode frame\n", filename.c_str());
			exit(1);
		}
		av_free_packet(&pkt);
	} while (!frame_finished);

	// TODO: Scale down if needed!
	AVPicture pic;
	avpicture_alloc(&pic, AV_PIX_FMT_RGBA, frame->width, frame->height);
	SwsContext *sws_ctx = sws_getContext(frame->width, frame->height,
		(AVPixelFormat)frame->format, frame->width, frame->height,
		AV_PIX_FMT_RGBA, SWS_BICUBIC, nullptr, nullptr, nullptr);
	if (sws_ctx == nullptr) {
		fprintf(stderr, "%s: Could not create scaler context\n", filename.c_str());
		exit(1);
	}
	sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height, pic.data, pic.linesize);
	sws_freeContext(sws_ctx);

	size_t len = frame->width * frame->height * 4;
	unique_ptr<uint8_t[]> image_data(new uint8_t[len]);
	av_image_copy_to_buffer(image_data.get(), len, pic.data, pic.linesize, AV_PIX_FMT_RGBA, frame->width, frame->height, 1);

	avpicture_free(&pic);
	av_frame_free(&frame);
	avcodec_close(codec_ctx);
	avformat_close_input(&format_ctx);

	all_images[filename] = move(image_data);
	return all_images[filename].get();
}

map<string, unique_ptr<uint8_t[]>> ImageInput::all_images;
