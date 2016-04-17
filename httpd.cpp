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

void HTTPD::add_packet(const AVPacket &pkt, int64_t pts, int64_t dts)
{
	unique_lock<mutex> lock(streams_mutex);
	for (Stream *stream : streams) {
		stream->add_packet(pkt, pts, dts);
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
	AVOutputFormat *oformat = av_guess_format(global_flags.stream_mux_name.c_str(), nullptr, nullptr);
	assert(oformat != nullptr);

	int time_base = global_flags.stream_coarse_timebase ? COARSE_TIMEBASE : TIMEBASE;
	HTTPD::Stream *stream = new HTTPD::Stream(oformat, width, height, time_base);
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

HTTPD::Stream::Stream(AVOutputFormat *oformat, int width, int height, int time_base)
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

	mux.reset(new Mux(avctx, width, height, video_codec, time_base));
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

