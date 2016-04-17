#ifndef _HTTPD_H
#define _HTTPD_H

// A class dealing with stream output, both to HTTP (thus the class name)
// and to local output files. Since we generally have very few outputs
// (end clients are not meant to connect directly to our stream; it should be
// transcoded by something else and then sent to a reflector), we don't need to
// care a lot about performance. Thus, we solve this by the simplest possible
// way, namely having one ffmpeg mux per output.

#include <microhttpd.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>

struct MHD_Connection;

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
}

#include "mux.h"

class HTTPD {
public:
	enum PacketDestination {
		DESTINATION_FILE_ONLY,
		DESTINATION_HTTP_ONLY,
		DESTINATION_FILE_AND_HTTP
	};

	HTTPD(int width, int height);
	void start(int port);
	void add_packet(const AVPacket &pkt, int64_t pts, int64_t dts, PacketDestination destination);

	// You can only have one going at the same time.
	void open_output_file(const std::string &filename);
	void close_output_file();

private:
	static int answer_to_connection_thunk(void *cls, MHD_Connection *connection,
	                                      const char *url, const char *method,
	                                      const char *version, const char *upload_data,
	                                      size_t *upload_data_size, void **con_cls);

	int answer_to_connection(MHD_Connection *connection,
	                         const char *url, const char *method,
	                         const char *version, const char *upload_data,
	                         size_t *upload_data_size, void **con_cls);

	static void free_stream(void *cls);

	static void request_completed_thunk(void *cls, struct MHD_Connection *connection, void **con_cls, enum MHD_RequestTerminationCode toe);

	void request_completed(struct MHD_Connection *connection, void **con_cls, enum MHD_RequestTerminationCode toe);


	class Stream {
	public:
		Stream(AVOutputFormat *oformat, int width, int height, int time_base);

		static ssize_t reader_callback_thunk(void *cls, uint64_t pos, char *buf, size_t max);
		ssize_t reader_callback(uint64_t pos, char *buf, size_t max);

		void add_packet(const AVPacket &pkt, int64_t pts, int64_t dts);

	private:
		static int write_packet_thunk(void *opaque, uint8_t *buf, int buf_size);
		int write_packet(uint8_t *buf, int buf_size);

		std::mutex buffer_mutex;
		std::condition_variable has_buffered_data;
		std::deque<std::string> buffered_data;  // Protected by <mutex>.
		size_t used_of_buffered_data = 0;  // How many bytes of the first element of <buffered_data> that is already used. Protected by <mutex>.

		std::unique_ptr<Mux> mux;  // Must come last to be destroyed before buffered_data, since the destructor can write bytes.
	};

	std::mutex streams_mutex;
	std::set<Stream *> streams;  // Not owned.

	int width, height;
	std::unique_ptr<Mux> file_mux;  // To local disk.
};

#endif  // !defined(_HTTPD_H)
