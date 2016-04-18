#ifndef _HTTPD_H
#define _HTTPD_H

// A class dealing with stream output to HTTP.

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

class HTTPD {
public:
	HTTPD();

	// Should be called before start().
	void set_header(const std::string &data) {
		header = data;
	}

	void start(int port);
	void add_data(const char *buf, size_t size, bool keyframe);

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
		static ssize_t reader_callback_thunk(void *cls, uint64_t pos, char *buf, size_t max);
		ssize_t reader_callback(uint64_t pos, char *buf, size_t max);

		enum DataType {
			DATA_TYPE_HEADER,
			DATA_TYPE_KEYFRAME,
			DATA_TYPE_OTHER
		};
		void add_data(const char *buf, size_t size, DataType data_type);

	private:
		std::mutex buffer_mutex;
		std::condition_variable has_buffered_data;
		std::deque<std::string> buffered_data;  // Protected by <mutex>.
		size_t used_of_buffered_data = 0;  // How many bytes of the first element of <buffered_data> that is already used. Protected by <mutex>.
		size_t seen_keyframe = false;
	};

	std::mutex streams_mutex;
	std::set<Stream *> streams;  // Not owned.
	std::string header;
};

#endif  // !defined(_HTTPD_H)
