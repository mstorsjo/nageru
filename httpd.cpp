#include <string.h>
#include <microhttpd.h>
#include <assert.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "httpd.h"
#include "timebase.h"

using namespace std;

HTTPD::HTTPD() {}

void HTTPD::start(int port)
{
	MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION | MHD_USE_POLL_INTERNALLY | MHD_USE_DUAL_STACK,
	                 port,
			 nullptr, nullptr,
			 &answer_to_connection_thunk, this, MHD_OPTION_END);
}

void HTTPD::add_packet(const AVPacket &pkt)
{
	for (Stream *stream : streams) {
		stream->add_packet(pkt);
	}
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
	printf("url %s\n", url);
	AVOutputFormat *oformat = av_guess_format("mpegts", nullptr, nullptr);
	assert(oformat != nullptr);
	HTTPD::Stream *stream = new HTTPD::Stream(oformat);
	streams.push_back(stream);
	MHD_Response *response = MHD_create_response_from_callback(
		(size_t)-1, 1048576, &HTTPD::Stream::reader_callback_thunk, stream, &HTTPD::free_stream);
	int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
	//MHD_destroy_response(response);

	return ret;
}

void HTTPD::free_stream(void *cls)
{
	HTTPD::Stream *stream = (HTTPD::Stream *)cls;
	delete stream;
}

HTTPD::Stream::Stream(AVOutputFormat *oformat)
{
	avctx = avformat_alloc_context();
	avctx->oformat = oformat;
	uint8_t *buf = (uint8_t *)av_malloc(1048576);
	avctx->pb = avio_alloc_context(buf, 1048576, 1, this, nullptr, &HTTPD::Stream::write_packet_thunk, nullptr);
	avctx->flags = AVFMT_FLAG_CUSTOM_IO;

	// TODO: Unify with the code in h264encoder.cpp.
	AVCodec *codec_video = avcodec_find_encoder(AV_CODEC_ID_H264);
	avstream_video = avformat_new_stream(avctx, codec_video);
	if (avstream_video == nullptr) {
		fprintf(stderr, "avformat_new_stream() failed\n");
		exit(1);
	}
	avstream_video->time_base = AVRational{1, TIMEBASE};
	avstream_video->codec->width = 1280;  // FIXME
	avstream_video->codec->height = 720;  // FIXME
	avstream_video->codec->time_base = AVRational{1, TIMEBASE};
	avstream_video->codec->ticks_per_frame = 1;  // or 2?

	AVCodec *codec_audio = avcodec_find_encoder(AV_CODEC_ID_MP3);
	avstream_audio = avformat_new_stream(avctx, codec_audio);
	if (avstream_audio == nullptr) {
		fprintf(stderr, "avformat_new_stream() failed\n");
		exit(1);
	}
	avstream_audio->time_base = AVRational{1, TIMEBASE};
	avstream_audio->codec->bit_rate = 256000;
	avstream_audio->codec->sample_rate = 48000;
	avstream_audio->codec->sample_fmt = AV_SAMPLE_FMT_FLTP;
	avstream_audio->codec->channels = 2;
	avstream_audio->codec->channel_layout = AV_CH_LAYOUT_STEREO;
	avstream_audio->codec->time_base = AVRational{1, TIMEBASE};

	if (avformat_write_header(avctx, NULL) < 0) {
		fprintf(stderr, "avformat_write_header() failed\n");
		exit(1);
	}
}

HTTPD::Stream::~Stream()
{
	avformat_free_context(avctx);
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
			ret += len;
			max -= len;
			buffered_data.pop_front();
			used_of_buffered_data = 0;
		} else {
			// We don't need the entire string; just use the first part of it.
			memcpy(buf, s.data() + used_of_buffered_data, max);
			used_of_buffered_data += max;
			ret += max;
			max = 0;
		}
	}

	return ret;
}

void HTTPD::Stream::add_packet(const AVPacket &pkt)
{
	AVPacket pkt_copy;
	av_copy_packet(&pkt_copy, &pkt);
	if (pkt.stream_index == 0) {
		pkt_copy.pts = av_rescale_q(pkt.pts, AVRational{1, TIMEBASE}, avstream_video->time_base);
		pkt_copy.dts = av_rescale_q(pkt.dts, AVRational{1, TIMEBASE}, avstream_video->time_base);
	} else if (pkt.stream_index == 1) {
		pkt_copy.pts = av_rescale_q(pkt.pts, AVRational{1, TIMEBASE}, avstream_audio->time_base);
		pkt_copy.dts = av_rescale_q(pkt.dts, AVRational{1, TIMEBASE}, avstream_audio->time_base);
	} else {
		assert(false);
	}

	if (av_interleaved_write_frame(avctx, &pkt_copy) < 0) {
		fprintf(stderr, "av_interleaved_write_frame() failed\n");
		exit(1);
	}
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

