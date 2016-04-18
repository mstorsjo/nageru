#include <arpa/inet.h>
#include <assert.h>
#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "httpd.h"

#include "defs.h"
#include "flags.h"
#include "metacube2.h"
#include "timebase.h"

struct MHD_Connection;
struct MHD_Response;

using namespace std;

HTTPD::HTTPD()
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

void HTTPD::add_data(const char *buf, size_t size, bool keyframe)
{
	unique_lock<mutex> lock(streams_mutex);
	for (Stream *stream : streams) {
		stream->add_data(buf, size, keyframe ? Stream::DATA_TYPE_KEYFRAME : Stream::DATA_TYPE_OTHER);
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
	// See if the URL ends in “.metacube”.
	HTTPD::Stream::Framing framing;
	if (strstr(url, ".metacube") == url + strlen(url) - strlen(".metacube")) {
		framing = HTTPD::Stream::FRAMING_METACUBE;
	} else {
		framing = HTTPD::Stream::FRAMING_RAW;
	}

	HTTPD::Stream *stream = new HTTPD::Stream(framing);
	stream->add_data(header.data(), header.size(), Stream::DATA_TYPE_HEADER);
	{
		unique_lock<mutex> lock(streams_mutex);
		streams.insert(stream);
	}
	*con_cls = stream;

	// Does not strictly have to be equal to MUX_BUFFER_SIZE.
	MHD_Response *response = MHD_create_response_from_callback(
		(size_t)-1, MUX_BUFFER_SIZE, &HTTPD::Stream::reader_callback_thunk, stream, &HTTPD::free_stream);

	// TODO: Content-type?
	if (framing == HTTPD::Stream::FRAMING_METACUBE) {
		MHD_add_response_header(response, "Content-encoding", "metacube");
	}
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

void HTTPD::Stream::add_data(const char *buf, size_t buf_size, HTTPD::Stream::DataType data_type)
{
	if (buf_size == 0) {
		return;
	}
	if (data_type == DATA_TYPE_KEYFRAME) {
		seen_keyframe = true;
	} else if (data_type == DATA_TYPE_OTHER && !seen_keyframe) {
		// Start sending only once we see a keyframe.
		return;
	}

	unique_lock<mutex> lock(buffer_mutex);

	if (framing == FRAMING_METACUBE) {
		metacube2_block_header hdr;
		memcpy(hdr.sync, METACUBE2_SYNC, sizeof(hdr.sync));
		hdr.size = htonl(buf_size);
		int flags = 0;
		if (data_type == DATA_TYPE_HEADER) {
			flags |= METACUBE_FLAGS_HEADER;
		} else if (data_type == DATA_TYPE_OTHER) {
			flags |= METACUBE_FLAGS_NOT_SUITABLE_FOR_STREAM_START;
		}
		hdr.flags = htons(flags);
		hdr.csum = htons(metacube2_compute_crc(&hdr));
		buffered_data.emplace_back((char *)&hdr, sizeof(hdr));
	}
	buffered_data.emplace_back(buf, buf_size);
	has_buffered_data.notify_all();	
}

