#include <assert.h>
#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
}

#include <vector>

#include "httpd.h"

#include "defs.h"
#include "flags.h"
#include "timebase.h"

struct MHD_Connection;
struct MHD_Response;

using namespace std;

HTTPD::HTTPD(int width, int height)
	: width(width), height(height)
{
}

void HTTPD::start(int port)
{
	MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION | MHD_USE_POLL_INTERNALLY | MHD_USE_DUAL_STACK,
	                 port,
			 nullptr, nullptr,
			 &answer_to_connection_thunk, this,
	                 MHD_OPTION_NOTIFY_COMPLETED, &request_completed_thunk, this, 
	                 MHD_OPTION_END);
}

void HTTPD::add_packet(const AVPacket &pkt, int64_t pts, int64_t dts, PacketDestination destination)
{
	unique_lock<mutex> lock(streams_mutex);
	if (destination != DESTINATION_FILE_ONLY) {
		for (Stream *stream : streams) {
			stream->add_packet(pkt, pts, dts);
		}
	}
	if (file_mux && destination != DESTINATION_HTTP_ONLY) {
		file_mux->add_packet(pkt, pts, dts);
	}
}

void HTTPD::open_output_file(const string &filename)
{
	AVFormatContext *avctx = avformat_alloc_context();
	avctx->oformat = av_guess_format(NULL, filename.c_str(), NULL);
	assert(filename.size() < sizeof(avctx->filename) - 1);
	strcpy(avctx->filename, filename.c_str());

	string url = "file:" + filename;
	int ret = avio_open2(&avctx->pb, url.c_str(), AVIO_FLAG_WRITE, &avctx->interrupt_callback, NULL);
	if (ret < 0) {
		char tmp[AV_ERROR_MAX_STRING_SIZE];
		fprintf(stderr, "%s: avio_open2() failed: %s\n", filename.c_str(), av_make_error_string(tmp, sizeof(tmp), ret));
		exit(1);
	}

	file_mux.reset(new Mux(avctx, width, height, Mux::CODEC_H264));
}

void HTTPD::close_output_file()
{
	file_mux.reset();
}

int HTTPD::answer_to_connection_thunk(void *cls, MHD_Connection *connection,
                                      const char *url, const char *method,
                                      const char *version, const char *upload_data,
                                      size_t *upload_data_size, void **con_cls)
{
	HTTPD *httpd = (HTTPD *)cls;
	return httpd->answer_to_connection(connection, url, method, version, upload_data, upload_data_size, con_cls);
}

int HTTPD::answer_to_connection(MHD_Connection *connection,
                                const char *url, const char *method,
				const char *version, const char *upload_data,
				size_t *upload_data_size, void **con_cls)
{
	AVOutputFormat *oformat = av_guess_format(STREAM_MUX_NAME, nullptr, nullptr);
	assert(oformat != nullptr);
	HTTPD::Stream *stream = new HTTPD::Stream(oformat, width, height);
	{
		unique_lock<mutex> lock(streams_mutex);
		streams.insert(stream);
	}
	*con_cls = stream;

	// Does not strictly have to be equal to MUX_BUFFER_SIZE.
	MHD_Response *response = MHD_create_response_from_callback(
		(size_t)-1, MUX_BUFFER_SIZE, &HTTPD::Stream::reader_callback_thunk, stream, &HTTPD::free_stream);
	int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
	//MHD_destroy_response(response);

	return ret;
}

void HTTPD::free_stream(void *cls)
{
	// FIXME: When is this actually called, if ever?
	// Also, shouldn't we remove it from streams?
	HTTPD::Stream *stream = (HTTPD::Stream *)cls;
	delete stream;
}

void HTTPD::request_completed_thunk(void *cls, struct MHD_Connection *connection, void **con_cls, enum MHD_RequestTerminationCode toe)
{
	HTTPD *httpd = (HTTPD *)cls;
	return httpd->request_completed(connection, con_cls, toe);
}

void HTTPD::request_completed(struct MHD_Connection *connection, void **con_cls, enum MHD_RequestTerminationCode toe)
{
	if (con_cls == nullptr) {
		// Request was never set up.
		return;
	}
	HTTPD::Stream *stream = (HTTPD::Stream *)*con_cls;
	{
		unique_lock<mutex> lock(streams_mutex);
		delete stream;
		streams.erase(stream);
	}
}

HTTPD::Mux::Mux(AVFormatContext *avctx, int width, int height, Codec video_codec)
	: avctx(avctx)
{
	AVCodec *codec_video = avcodec_find_encoder((video_codec == CODEC_H264) ? AV_CODEC_ID_H264 : AV_CODEC_ID_RAWVIDEO);
	avstream_video = avformat_new_stream(avctx, codec_video);
	if (avstream_video == nullptr) {
		fprintf(stderr, "avformat_new_stream() failed\n");
		exit(1);
	}
	avstream_video->time_base = AVRational{1, TIMEBASE};
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
	avstream_video->codec->time_base = AVRational{1, TIMEBASE};
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

	AVCodec *codec_audio = avcodec_find_encoder(AUDIO_OUTPUT_CODEC);
	avstream_audio = avformat_new_stream(avctx, codec_audio);
	if (avstream_audio == nullptr) {
		fprintf(stderr, "avformat_new_stream() failed\n");
		exit(1);
	}
	avstream_audio->time_base = AVRational{1, TIMEBASE};
	avstream_audio->codec->bit_rate = AUDIO_OUTPUT_BIT_RATE;
	avstream_audio->codec->sample_rate = OUTPUT_FREQUENCY;
	avstream_audio->codec->sample_fmt = AUDIO_OUTPUT_SAMPLE_FMT;
	avstream_audio->codec->channels = 2;
	avstream_audio->codec->channel_layout = AV_CH_LAYOUT_STEREO;
	avstream_audio->codec->time_base = AVRational{1, TIMEBASE};
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
}

HTTPD::Mux::~Mux()
{
	av_write_trailer(avctx);
	av_free(avctx->pb->buffer);
	av_free(avctx->pb);
	avformat_free_context(avctx);
}

void HTTPD::Mux::add_packet(const AVPacket &pkt, int64_t pts, int64_t dts)
{
	if (!seen_keyframe && !(pkt.stream_index == 0 && (pkt.flags & AV_PKT_FLAG_KEY))) {
		// Wait until we see the first (video) key frame.
		return;
	}
	seen_keyframe = true;

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

	if (av_interleaved_write_frame(avctx, &pkt_copy) < 0) {
		fprintf(stderr, "av_interleaved_write_frame() failed\n");
		exit(1);
	}

	av_packet_unref(&pkt_copy);
}

HTTPD::Stream::Stream(AVOutputFormat *oformat, int width, int height)
{
	AVFormatContext *avctx = avformat_alloc_context();
	avctx->oformat = oformat;
	uint8_t *buf = (uint8_t *)av_malloc(MUX_BUFFER_SIZE);
	avctx->pb = avio_alloc_context(buf, MUX_BUFFER_SIZE, 1, this, nullptr, &HTTPD::Stream::write_packet_thunk, nullptr);

	Mux::Codec video_codec;
	if (global_flags.uncompressed_video_to_http) {
		video_codec = Mux::CODEC_NV12;
	} else {
		video_codec = Mux::CODEC_H264;
	}

	avctx->flags = AVFMT_FLAG_CUSTOM_IO;

	mux.reset(new Mux(avctx, width, height, video_codec));
}

ssize_t HTTPD::Stream::reader_callback_thunk(void *cls, uint64_t pos, char *buf, size_t max)
{
	HTTPD::Stream *stream = (HTTPD::Stream *)cls;
	return stream->reader_callback(pos, buf, max);
}

ssize_t HTTPD::Stream::reader_callback(uint64_t pos, char *buf, size_t max)
{
	unique_lock<mutex> lock(buffer_mutex);
	has_buffered_data.wait(lock, [this]{ return !buffered_data.empty(); });

	ssize_t ret = 0;
	while (max > 0 && !buffered_data.empty()) {
		const string &s = buffered_data.front();
		assert(s.size() > used_of_buffered_data);
		size_t len = s.size() - used_of_buffered_data;
		if (max >= len) {
			// Consume the entire (rest of the) string.
			memcpy(buf, s.data() + used_of_buffered_data, len);
			buf += len;
			ret += len;
			max -= len;
			buffered_data.pop_front();
			used_of_buffered_data = 0;
		} else {
			// We don't need the entire string; just use the first part of it.
			memcpy(buf, s.data() + used_of_buffered_data, max);
			buf += max;
			used_of_buffered_data += max;
			ret += max;
			max = 0;
		}
	}

	return ret;
}

void HTTPD::Stream::add_packet(const AVPacket &pkt, int64_t pts, int64_t dts)
{
	mux->add_packet(pkt, pts, dts);
}

int HTTPD::Stream::write_packet_thunk(void *opaque, uint8_t *buf, int buf_size)
{
	HTTPD::Stream *stream = (HTTPD::Stream *)opaque;
	return stream->write_packet(buf, buf_size);
}

int HTTPD::Stream::write_packet(uint8_t *buf, int buf_size)
{
	unique_lock<mutex> lock(buffer_mutex);
	buffered_data.emplace_back((char *)buf, buf_size);
	has_buffered_data.notify_all();	
	return buf_size;
}

