#ifndef _HTTPD_H
#define _HTTPD_H

#include <microhttpd.h>
#include <deque>
#include <string>
#include <mutex>
#include <condition_variable>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

class HTTPD {
public:
	HTTPD();
	void start(int port);
	void add_packet(const AVPacket &pkt);

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

	class Stream {
	public:
		Stream(AVOutputFormat *oformat);
		~Stream();

		static ssize_t reader_callback_thunk(void *cls, uint64_t pos, char *buf, size_t max);
		ssize_t reader_callback(uint64_t pos, char *buf, size_t max);

		void add_packet(const AVPacket &pkt);

	private:
		static int write_packet_thunk(void *opaque, uint8_t *buf, int buf_size);
		int write_packet(uint8_t *buf, int buf_size);

		AVIOContext *avio;
		AVFormatContext *avctx;
		AVStream *avstream_video, *avstream_audio;

		std::mutex buffer_mutex;
		std::condition_variable has_buffered_data;
		std::deque<std::string> buffered_data;  // Protected by <mutex>.
		size_t used_of_buffered_data = 0;  // How many bytes of the first element of <buffered_data> that is already used. Protected by <mutex>.
	};

	std::vector<Stream *> streams;  // Not owned.
};

#endif  // !defined(_HTTPD_H)
