#include <assert.h>

#include <string>
#include <vector>

#include "defs.h"
#include "mux.h"
#include "timebase.h"

using namespace std;

Mux::Mux(AVFormatContext *avctx, int width, int height, Codec video_codec, const AVCodec *codec_audio, int time_base, int bit_rate, KeyFrameSignalReceiver *keyframe_signal_receiver)
	: avctx(avctx), keyframe_signal_receiver(keyframe_signal_receiver)
{
	AVCodec *codec_video = avcodec_find_encoder((video_codec == CODEC_H264) ? AV_CODEC_ID_H264 : AV_CODEC_ID_RAWVIDEO);
	avstream_video = avformat_new_stream(avctx, codec_video);
	if (avstream_video == nullptr) {
		fprintf(stderr, "avformat_new_stream() failed\n");
		exit(1);
	}
	avstream_video->time_base = AVRational{1, time_base};
	avstream_video->codec->codec_type = AVMEDIA_TYPE_VIDEO;
	if (video_codec == CODEC_H264) {
		avstream_video->codec->codec_id = AV_CODEC_ID_H264;
	} else {
		assert(video_codec == CODEC_NV12);
		avstream_video->codec->codec_id = AV_CODEC_ID_RAWVIDEO;
		avstream_video->codec->codec_tag = avcodec_pix_fmt_to_codec_tag(AV_PIX_FMT_NV12);
	}
	avstream_video->codec->width = width;
	avstream_video->codec->height = height;
	avstream_video->codec->time_base = AVRational{1, time_base};
	avstream_video->codec->ticks_per_frame = 1;  // or 2?

	// Colorspace details. Closely correspond to settings in EffectChain_finalize,
	// as noted in each comment.
	// Note that the H.264 stream also contains this information and depending on the
	// mux, this might simply get ignored. See sps_rbsp().
	avstream_video->codec->color_primaries = AVCOL_PRI_BT709;  // RGB colorspace (inout_format.color_space).
	avstream_video->codec->color_trc = AVCOL_TRC_UNSPECIFIED;  // Gamma curve (inout_format.gamma_curve).
	avstream_video->codec->colorspace = AVCOL_SPC_SMPTE170M;  // YUV colorspace (output_ycbcr_format.luma_coefficients).
	avstream_video->codec->color_range = AVCOL_RANGE_MPEG;  // Full vs. limited range (output_ycbcr_format.full_range).
	avstream_video->codec->chroma_sample_location = AVCHROMA_LOC_LEFT;  // Chroma sample location. See chroma_offset_0[] in Mixer::subsample_chroma().
	avstream_video->codec->field_order = AV_FIELD_PROGRESSIVE;
	if (avctx->oformat->flags & AVFMT_GLOBALHEADER) {
		avstream_video->codec->flags = AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	avstream_audio = avformat_new_stream(avctx, codec_audio);
	if (avstream_audio == nullptr) {
		fprintf(stderr, "avformat_new_stream() failed\n");
		exit(1);
	}
	avstream_audio->time_base = AVRational{1, time_base};
	avstream_audio->codec->bit_rate = bit_rate;
	avstream_audio->codec->sample_rate = OUTPUT_FREQUENCY;
	avstream_audio->codec->channels = 2;
	avstream_audio->codec->channel_layout = AV_CH_LAYOUT_STEREO;
	avstream_audio->codec->time_base = AVRational{1, time_base};
	if (avctx->oformat->flags & AVFMT_GLOBALHEADER) {
		avstream_audio->codec->flags = AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	AVDictionary *options = NULL;
	vector<pair<string, string>> opts = MUX_OPTS;
	for (pair<string, string> opt : opts) {
		av_dict_set(&options, opt.first.c_str(), opt.second.c_str(), 0);
	}
	if (avformat_write_header(avctx, &options) < 0) {
		fprintf(stderr, "avformat_write_header() failed\n");
		exit(1);
	}

	// Make sure the header is written before the constructor exits.
	avio_flush(avctx->pb);
}

Mux::~Mux()
{
	av_write_trailer(avctx);
	av_free(avctx->pb->buffer);
	av_free(avctx->pb);
	avformat_free_context(avctx);
}

void Mux::add_packet(const AVPacket &pkt, int64_t pts, int64_t dts)
{
	AVPacket pkt_copy;
	av_copy_packet(&pkt_copy, &pkt);
	if (pkt.stream_index == 0) {
		pkt_copy.pts = av_rescale_q(pts, AVRational{1, TIMEBASE}, avstream_video->time_base);
		pkt_copy.dts = av_rescale_q(dts, AVRational{1, TIMEBASE}, avstream_video->time_base);
	} else if (pkt.stream_index == 1) {
		pkt_copy.pts = av_rescale_q(pts, AVRational{1, TIMEBASE}, avstream_audio->time_base);
		pkt_copy.dts = av_rescale_q(dts, AVRational{1, TIMEBASE}, avstream_audio->time_base);
	} else {
		assert(false);
	}

	if (keyframe_signal_receiver) {
		if (pkt.flags & AV_PKT_FLAG_KEY) {
			if (avctx->oformat->flags & AVFMT_ALLOW_FLUSH) {
				av_write_frame(avctx, nullptr);
			}
			keyframe_signal_receiver->signal_keyframe();
		}
	}

	if (av_interleaved_write_frame(avctx, &pkt_copy) < 0) {
		fprintf(stderr, "av_interleaved_write_frame() failed\n");
		exit(1);
	}

	av_packet_unref(&pkt_copy);
}
