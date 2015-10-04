// TODO: Replace with linking to upstream bmusb.

// Intensity Shuttle USB3 prototype capture driver, v0.3
// Can download 8-bit and 10-bit UYVY/v210 frames from HDMI, quite stable
// (can do captures for hours at a time with no drops), except during startup
// 576p60/720p60/1080i60 works, 1080p60 does not work (firmware limitation)
// Audio comes out as 8-channel 24-bit raw audio.

#include <assert.h>
#include <errno.h>
#include <libusb.h>
#include <netinet/in.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __SSE2__
#include <immintrin.h>
#endif
#include "bmusb.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stack>
#include <thread>

using namespace std;
using namespace std::placeholders;

#define WIDTH 1280
#define HEIGHT 750  /* 30 lines ancillary data? */
//#define WIDTH 1920
//#define HEIGHT 1125  /* ??? lines ancillary data? */
#define HEADER_SIZE 44
//#define HEADER_SIZE 0
#define AUDIO_HEADER_SIZE 4

//#define FRAME_SIZE (WIDTH * HEIGHT * 2 + HEADER_SIZE)  // UYVY
//#define FRAME_SIZE (WIDTH * HEIGHT * 2 * 4 / 3 + HEADER_SIZE)  // v210
#define FRAME_SIZE (8 << 20)

FILE *audiofp;

thread usb_thread;
atomic<bool> should_quit;

FrameAllocator::~FrameAllocator() {}

#define NUM_QUEUED_FRAMES 16
class MallocFrameAllocator : public FrameAllocator {
public:
	MallocFrameAllocator(size_t frame_size);
	Frame alloc_frame() override;
	void release_frame(Frame frame) override;

private:
	size_t frame_size;

	mutex freelist_mutex;
	stack<unique_ptr<uint8_t[]>> freelist;  // All of size <frame_size>.
};

MallocFrameAllocator::MallocFrameAllocator(size_t frame_size)
	: frame_size(frame_size)
{
	for (int i = 0; i < NUM_QUEUED_FRAMES; ++i) {
		freelist.push(unique_ptr<uint8_t[]>(new uint8_t[frame_size]));
	}
}

FrameAllocator::Frame MallocFrameAllocator::alloc_frame()
{
	Frame vf;
	vf.owner = this;

	unique_lock<mutex> lock(freelist_mutex);  // Meh.
	if (freelist.empty()) {
		printf("Frame overrun (no more spare frames of size %ld), dropping frame!\n",
			frame_size);
	} else {
		vf.data = freelist.top().release();
		vf.size = frame_size;
		freelist.pop();  // Meh.
	}
	return vf;
}

void MallocFrameAllocator::release_frame(Frame frame)
{
	unique_lock<mutex> lock(freelist_mutex);
	freelist.push(unique_ptr<uint8_t[]>(frame.data));
}

bool uint16_less_than_with_wraparound(uint16_t a, uint16_t b)
{
	if (a == b) {
		return false;
	} else if (a < b) {
		return (b - a < 0x8000);
	} else {
		int wrap_b = 0x10000 + int(b);
		return (wrap_b - a < 0x8000);
	}
}

void BMUSBCapture::queue_frame(uint16_t format, uint16_t timecode, FrameAllocator::Frame frame, deque<QueuedFrame> *q)
{
	if (!q->empty() && !uint16_less_than_with_wraparound(q->back().timecode, timecode)) {
		printf("Blocks going backwards: prev=0x%04x, cur=0x%04x (dropped)\n",
			q->back().timecode, timecode);
		frame.owner->release_frame(frame);
		return;
	}

	QueuedFrame qf;
	qf.format = format;
	qf.timecode = timecode;
	qf.frame = frame;

	{
		unique_lock<mutex> lock(queue_lock);
		q->push_back(move(qf));
	}
	queues_not_empty.notify_one();  // might be spurious
}

void dump_frame(const char *filename, uint8_t *frame_start, size_t frame_len)
{
	FILE *fp = fopen(filename, "wb");
	if (fwrite(frame_start + HEADER_SIZE, frame_len - HEADER_SIZE, 1, fp) != 1) {
		printf("short write!\n");
	}
	fclose(fp);
}

void dump_audio_block(uint8_t *audio_start, size_t audio_len)
{
	fwrite(audio_start + AUDIO_HEADER_SIZE, 1, audio_len - AUDIO_HEADER_SIZE, audiofp);
}

void BMUSBCapture::dequeue_thread()
{
	for ( ;; ) {
		unique_lock<mutex> lock(queue_lock);
		queues_not_empty.wait(lock, [this]{ return !pending_video_frames.empty() && !pending_audio_frames.empty(); });

		uint16_t video_timecode = pending_video_frames.front().timecode;
		uint16_t audio_timecode = pending_audio_frames.front().timecode;
		if (video_timecode < audio_timecode) {
			printf("Video block 0x%04x without corresponding audio block, dropping.\n",
				video_timecode);
			video_frame_allocator->release_frame(pending_video_frames.front().frame);
			pending_video_frames.pop_front();
		} else if (audio_timecode < video_timecode) {
			printf("Audio block 0x%04x without corresponding video block, dropping.\n",
				audio_timecode);
			audio_frame_allocator->release_frame(pending_audio_frames.front().frame);
			pending_audio_frames.pop_front();
		} else {
			QueuedFrame video_frame = pending_video_frames.front();
			QueuedFrame audio_frame = pending_audio_frames.front();
			pending_audio_frames.pop_front();
			pending_video_frames.pop_front();
			lock.unlock();

#if 0
			char filename[255];
			snprintf(filename, sizeof(filename), "%04x%04x.uyvy", video_frame.format, video_timecode);
			dump_frame(filename, video_frame.frame.data, video_frame.data_len);
			dump_audio_block(audio_frame.frame.data, audio_frame.data_len); 
#endif

			frame_callback(video_timecode,
			               video_frame.frame, HEADER_SIZE, video_frame.format,
			               audio_frame.frame, AUDIO_HEADER_SIZE, audio_frame.format);
		}
	}
}

void BMUSBCapture::start_new_frame(const uint8_t *start)
{
	uint16_t format = (start[3] << 8) | start[2];
	uint16_t timecode = (start[1] << 8) | start[0];

	if (current_video_frame.len > 0) {
		//dump_frame();
		queue_frame(format, timecode, current_video_frame, &pending_video_frames);
	}
	//printf("Found frame start, format 0x%04x timecode 0x%04x, previous frame length was %d/%d\n",
	//	format, timecode,
	//	//start[7], start[6], start[5], start[4],
	//	read_current_frame, FRAME_SIZE);

	current_video_frame = video_frame_allocator->alloc_frame();
	//if (current_video_frame.data == nullptr) {
	//	read_current_frame = -1;
	//} else {
	//	read_current_frame = 0;
	//}
}

void BMUSBCapture::start_new_audio_block(const uint8_t *start)
{
	uint16_t format = (start[3] << 8) | start[2];
	uint16_t timecode = (start[1] << 8) | start[0];
	if (current_audio_frame.len > 0) {
		//dump_audio_block();
		queue_frame(format, timecode, current_audio_frame, &pending_audio_frames);
	}
	//printf("Found audio block start, format 0x%04x timecode 0x%04x, previous block length was %d\n",
	//	format, timecode, read_current_audio_block);
	current_audio_frame = audio_frame_allocator->alloc_frame();
}

#if 0
static void dump_pack(const libusb_transfer *xfr, int offset, const libusb_iso_packet_descriptor *pack)
{
	//	printf("ISO pack%u length:%u, actual_length:%u, offset:%u\n", i, pack->length, pack->actual_length, offset);
	for (unsigned j = 0; j < pack->actual_length; j++) {
	//for (int j = 0; j < min(pack->actual_length, 16u); j++) {
		printf("%02x", xfr->buffer[j + offset]);
		if ((j % 16) == 15)
			printf("\n");
		else if ((j % 8) == 7)
			printf("  ");
		else
			printf(" ");
	}
}
#endif

void memcpy_interleaved(uint8_t *dest1, uint8_t *dest2, const uint8_t *src, size_t n)
{
	assert(n % 2 == 0);
	uint8_t *dptr1 = dest1;
	uint8_t *dptr2 = dest2;

	for (size_t i = 0; i < n; i += 2) {
		*dptr1++ = *src++;
		*dptr2++ = *src++;
	}
}

void add_to_frame(FrameAllocator::Frame *current_frame, const char *frame_type_name, const uint8_t *start, const uint8_t *end)
{
	if (current_frame->data == nullptr ||
	    current_frame->len > current_frame->size ||
	    start == end) {
		return;
	}

	int bytes = end - start;
	if (current_frame->len + bytes > current_frame->size) {
		printf("%d bytes overflow after last %s frame\n",
			int(current_frame->len + bytes - current_frame->size), frame_type_name);
		//dump_frame();
	} else {
		if (current_frame->interleaved) {
			uint8_t *data = current_frame->data + current_frame->len / 2;
			uint8_t *data2 = current_frame->data2 + current_frame->len / 2;
			if (current_frame->len % 2 == 1) {
				++data;
				swap(data, data2);
			}
			if (bytes % 2 == 1) {
				*data++ = *start++;
				swap(data, data2);
				++current_frame->len;
				--bytes;
			}
			memcpy_interleaved(data, data2, start, bytes);
			current_frame->len += bytes;
		} else {
			memcpy(current_frame->data + current_frame->len, start, bytes);
			current_frame->len += bytes;
		}
	}
}

#ifdef __SSE2__

#if 0
void dump(const char *name, __m256i n)
{
	printf("%-10s:", name);
	printf(" %02x", _mm256_extract_epi8(n, 0));
	printf(" %02x", _mm256_extract_epi8(n, 1));
	printf(" %02x", _mm256_extract_epi8(n, 2));
	printf(" %02x", _mm256_extract_epi8(n, 3));
	printf(" %02x", _mm256_extract_epi8(n, 4));
	printf(" %02x", _mm256_extract_epi8(n, 5));
	printf(" %02x", _mm256_extract_epi8(n, 6));
	printf(" %02x", _mm256_extract_epi8(n, 7));
	printf(" ");
	printf(" %02x", _mm256_extract_epi8(n, 8));
	printf(" %02x", _mm256_extract_epi8(n, 9));
	printf(" %02x", _mm256_extract_epi8(n, 10));
	printf(" %02x", _mm256_extract_epi8(n, 11));
	printf(" %02x", _mm256_extract_epi8(n, 12));
	printf(" %02x", _mm256_extract_epi8(n, 13));
	printf(" %02x", _mm256_extract_epi8(n, 14));
	printf(" %02x", _mm256_extract_epi8(n, 15));
	printf(" ");
	printf(" %02x", _mm256_extract_epi8(n, 16));
	printf(" %02x", _mm256_extract_epi8(n, 17));
	printf(" %02x", _mm256_extract_epi8(n, 18));
	printf(" %02x", _mm256_extract_epi8(n, 19));
	printf(" %02x", _mm256_extract_epi8(n, 20));
	printf(" %02x", _mm256_extract_epi8(n, 21));
	printf(" %02x", _mm256_extract_epi8(n, 22));
	printf(" %02x", _mm256_extract_epi8(n, 23));
	printf(" ");
	printf(" %02x", _mm256_extract_epi8(n, 24));
	printf(" %02x", _mm256_extract_epi8(n, 25));
	printf(" %02x", _mm256_extract_epi8(n, 26));
	printf(" %02x", _mm256_extract_epi8(n, 27));
	printf(" %02x", _mm256_extract_epi8(n, 28));
	printf(" %02x", _mm256_extract_epi8(n, 29));
	printf(" %02x", _mm256_extract_epi8(n, 30));
	printf(" %02x", _mm256_extract_epi8(n, 31));
	printf("\n");
}
#endif

// Does a memcpy and memchr in one to reduce processing time.
// Note that the benefit is somewhat limited if your L3 cache is small,
// as you'll (unfortunately) spend most of the time loading the data
// from main memory.
//
// Complicated cases are left to the slow path; it basically stops copying
// up until the first instance of "sync_char" (usually a bit before, actually).
// This is fine, since 0x00 bytes shouldn't really show up in normal picture
// data, and what we really need this for is the 00 00 ff ff marker in video data.
const uint8_t *add_to_frame_fastpath(FrameAllocator::Frame *current_frame, const uint8_t *start, const uint8_t *limit, const char sync_char)
{
	if (current_frame->data == nullptr ||
	    current_frame->len > current_frame->size ||
	    start == limit) {
		return start;
	}
	size_t orig_bytes = limit - start;
	if (orig_bytes < 128) {
		// Don't bother.
		return start;
	}

	// Don't read more bytes than we can write.
	limit = min(limit, start + (current_frame->size - current_frame->len));

	// Align end to 32 bytes.
	limit = (const uint8_t *)(intptr_t(limit) & ~31);

	if (start >= limit) {
		return start;
	}

	// Process [0,31] bytes, such that start gets aligned to 32 bytes.
	const uint8_t *aligned_start = (const uint8_t *)(intptr_t(start + 31) & ~31);
	if (aligned_start != start) {
		const uint8_t *sync_start = (const uint8_t *)memchr(start, sync_char, aligned_start - start);
		if (sync_start == nullptr) {
			add_to_frame(current_frame, "", start, aligned_start);
		} else {
			add_to_frame(current_frame, "", start, sync_start);
			return sync_start;
		}
	}

	// Make the length a multiple of 64.
	if (current_frame->interleaved) {
		if (((limit - aligned_start) % 64) != 0) {
			limit -= 32;
		}
		assert(((limit - aligned_start) % 64) == 0);
	}

#if __AVX2__
	const __m256i needle = _mm256_set1_epi8(sync_char);

	const __restrict __m256i *in = (const __m256i *)aligned_start;
	if (current_frame->interleaved) {
		__restrict __m256i *out1 = (__m256i *)(current_frame->data + (current_frame->len + 1) / 2);
		__restrict __m256i *out2 = (__m256i *)(current_frame->data2 + current_frame->len / 2);
		if (current_frame->len % 2 == 1) {
			swap(out1, out2);
		}

		__m256i shuffle_cw = _mm256_set_epi8(
			15, 13, 11, 9, 7, 5, 3, 1, 14, 12, 10, 8, 6, 4, 2, 0,
			15, 13, 11, 9, 7, 5, 3, 1, 14, 12, 10, 8, 6, 4, 2, 0);
		while (in < (const __m256i *)limit) {
			// Note: For brevity, comments show lanes as if they were 2x64-bit (they're actually 2x128).
			__m256i data1 = _mm256_stream_load_si256(in);         // AaBbCcDd EeFfGgHh
			__m256i data2 = _mm256_stream_load_si256(in + 1);     // IiJjKkLl MmNnOoPp

			__m256i found1 = _mm256_cmpeq_epi8(data1, needle);
			__m256i found2 = _mm256_cmpeq_epi8(data2, needle);
			__m256i found = _mm256_or_si256(found1, found2);

			data1 = _mm256_shuffle_epi8(data1, shuffle_cw);       // ABCDabcd EFGHefgh
			data2 = _mm256_shuffle_epi8(data2, shuffle_cw);       // IJKLijkl MNOPmnop
		
			data1 = _mm256_permute4x64_epi64(data1, 0b11011000);  // ABCDEFGH abcdefgh
			data2 = _mm256_permute4x64_epi64(data2, 0b11011000);  // IJKLMNOP ijklmnop

			__m256i lo = _mm256_permute2x128_si256(data1, data2, 0b00100000);
			__m256i hi = _mm256_permute2x128_si256(data1, data2, 0b00110001);

			_mm256_storeu_si256(out1, lo);  // Store as early as possible, even if the data isn't used.
			_mm256_storeu_si256(out2, hi);

			if (!_mm256_testz_si256(found, found)) {
				break;
			}

			in += 2;
			++out1;
			++out2;
		}
		current_frame->len += (uint8_t *)in - aligned_start;
	} else {
		__m256i *out = (__m256i *)(current_frame->data + current_frame->len);
		while (in < (const __m256i *)limit) {
			__m256i data = _mm256_load_si256(in);
			_mm256_storeu_si256(out, data);  // Store as early as possible, even if the data isn't used.
			__m256i found = _mm256_cmpeq_epi8(data, needle);
			if (!_mm256_testz_si256(found, found)) {
				break;
			}

			++in;
			++out;
		}
		current_frame->len = (uint8_t *)out - current_frame->data;
	}
#else
	const __m128i needle = _mm_set1_epi8(sync_char);

	const __m128i *in = (const __m128i *)aligned_start;
	if (current_frame->interleaved) {
		__m128i *out1 = (__m128i *)(current_frame->data + (current_frame->len + 1) / 2);
		__m128i *out2 = (__m128i *)(current_frame->data2 + current_frame->len / 2);
		if (current_frame->len % 2 == 1) {
			swap(out1, out2);
		}

		__m128i mask_lower_byte = _mm_set1_epi16(0x00ff);
		while (in < (const __m128i *)limit) {
			__m128i data1 = _mm_load_si128(in);
			__m128i data2 = _mm_load_si128(in + 1);
			__m128i data1_lo = _mm_and_si128(data1, mask_lower_byte);
			__m128i data2_lo = _mm_and_si128(data2, mask_lower_byte);
			__m128i data1_hi = _mm_srli_epi16(data1, 8);
			__m128i data2_hi = _mm_srli_epi16(data2, 8);
			__m128i lo = _mm_packus_epi16(data1_lo, data2_lo);
			_mm_storeu_si128(out1, lo);  // Store as early as possible, even if the data isn't used.
			__m128i hi = _mm_packus_epi16(data1_hi, data2_hi);
			_mm_storeu_si128(out2, hi);
			__m128i found1 = _mm_cmpeq_epi8(data1, needle);
			__m128i found2 = _mm_cmpeq_epi8(data2, needle);
			if (!_mm_testz_si128(found1, found1) ||
			    !_mm_testz_si128(found2, found2)) {
				break;
			}

			in += 2;
			++out1;
			++out2;
		}
		current_frame->len += (uint8_t *)in - aligned_start;
	} else {
		__m128i *out = (__m128i *)(current_frame->data + current_frame->len);
		while (in < (const __m128i *)limit) {
			__m128i data = _mm_load_si128(in);
			_mm_storeu_si128(out, data);  // Store as early as possible, even if the data isn't used.
			__m128i found = _mm_cmpeq_epi8(data, needle);
			if (!_mm_testz_si128(found, found)) {
				break;
			}

			++in;
			++out;
		}
		current_frame->len = (uint8_t *)out - current_frame->data;
	}
#endif

	//printf("managed to fastpath %ld/%ld bytes\n", (const uint8_t *)in - (const uint8_t *)aligned_start, orig_bytes);

	return (const uint8_t *)in;
}
#endif

void decode_packs(const libusb_transfer *xfr,
                  const char *sync_pattern,
                  int sync_length,
                  FrameAllocator::Frame *current_frame,
                  const char *frame_type_name,
                  function<void(const uint8_t *start)> start_callback)
{
	int offset = 0;
	for (int i = 0; i < xfr->num_iso_packets; i++) {
		const libusb_iso_packet_descriptor *pack = &xfr->iso_packet_desc[i];

		if (pack->status != LIBUSB_TRANSFER_COMPLETED) {
			fprintf(stderr, "Error: pack %u/%u status %d\n", i, xfr->num_iso_packets, pack->status);
			continue;
//exit(5);
		}

		const uint8_t *start = xfr->buffer + offset;
		const uint8_t *limit = start + pack->actual_length;
		while (start < limit) {  // Usually runs only one iteration.
#ifdef __SSE2__
			start = add_to_frame_fastpath(current_frame, start, limit, sync_pattern[0]);
			if (start == limit) break;
			assert(start < limit);
#endif

			const unsigned char* start_next_frame = (const unsigned char *)memmem(start, limit - start, sync_pattern, sync_length);
			if (start_next_frame == nullptr) {
				// add the rest of the buffer
				add_to_frame(current_frame, frame_type_name, start, limit);
				break;
			} else {
				add_to_frame(current_frame, frame_type_name, start, start_next_frame);
				start = start_next_frame + sync_length;  // skip sync
				start_callback(start);
			}
		}
#if 0
		dump_pack(xfr, offset, pack);
#endif
		offset += pack->length;
	}
}

void BMUSBCapture::cb_xfr(struct libusb_transfer *xfr)
{
	if (xfr->status != LIBUSB_TRANSFER_COMPLETED) {
		fprintf(stderr, "transfer status %d\n", xfr->status);
		libusb_free_transfer(xfr);
		exit(3);
	}

	assert(xfr->user_data != nullptr);
	BMUSBCapture *usb = static_cast<BMUSBCapture *>(xfr->user_data);

	if (xfr->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
		if (xfr->endpoint == 0x84) {
			decode_packs(xfr, "DeckLinkAudioResyncT", 20, &usb->current_audio_frame, "audio", bind(&BMUSBCapture::start_new_audio_block, usb, _1));
		} else {
			decode_packs(xfr, "\x00\x00\xff\xff", 4, &usb->current_video_frame, "video", bind(&BMUSBCapture::start_new_frame, usb, _1));
		}
	}
	if (xfr->type == LIBUSB_TRANSFER_TYPE_CONTROL) {
		//const libusb_control_setup *setup = libusb_control_transfer_get_setup(xfr);
		uint8_t *buf = libusb_control_transfer_get_data(xfr);
#if 0
		if (setup->wIndex == 44) {
			printf("read timer register: 0x%02x%02x%02x%02x\n", buf[0], buf[1], buf[2], buf[3]);
		} else {
			printf("read register %2d:                      0x%02x%02x%02x%02x\n",
				setup->wIndex, buf[0], buf[1], buf[2], buf[3]);
		}
#else
		memcpy(usb->register_file + usb->current_register, buf, 4);
		usb->current_register = (usb->current_register + 4) % NUM_BMUSB_REGISTERS;
		if (usb->current_register == 0) {
			// read through all of them
			printf("register dump:");
			for (int i = 0; i < NUM_BMUSB_REGISTERS; i += 4) {
				printf(" 0x%02x%02x%02x%02x", usb->register_file[i], usb->register_file[i + 1], usb->register_file[i + 2], usb->register_file[i + 3]);
			}
			printf("\n");
		}
		libusb_fill_control_setup(xfr->buffer,
		    LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN, /*request=*/214, /*value=*/0,
			/*index=*/usb->current_register, /*length=*/4);
#endif
	}

#if 0
	printf("length:%u, actual_length:%u\n", xfr->length, xfr->actual_length);
	for (i = 0; i < xfr->actual_length; i++) {
		printf("%02x", xfr->buffer[i]);
		if (i % 16)
			printf("\n");
		else if (i % 8)
			printf("  ");
		else
			printf(" ");
	}
#endif

	if (libusb_submit_transfer(xfr) < 0) {
		fprintf(stderr, "error re-submitting URB\n");
		exit(1);
	}
}

void BMUSBCapture::usb_thread_func()
{
	sched_param param;
	memset(&param, 0, sizeof(param));
	param.sched_priority = 1;
	if (sched_setscheduler(0, SCHED_RR, &param) == -1) {
		printf("couldn't set realtime priority for USB thread: %s\n", strerror(errno));
	}
	while (!should_quit) {
		int rc = libusb_handle_events(nullptr);
		if (rc != LIBUSB_SUCCESS)
			break;
	}
}

void BMUSBCapture::configure_card()
{
	if (video_frame_allocator == nullptr) {
		set_video_frame_allocator(new MallocFrameAllocator(FRAME_SIZE));  // FIXME: leak.
	}
	if (audio_frame_allocator == nullptr) {
		set_audio_frame_allocator(new MallocFrameAllocator(65536));  // FIXME: leak.
	}
	thread(&BMUSBCapture::dequeue_thread, this).detach();

	int rc;
	struct libusb_transfer *xfr;

	rc = libusb_init(nullptr);
	if (rc < 0) {
		fprintf(stderr, "Error initializing libusb: %s\n", libusb_error_name(rc));
		exit(1);
	}

	//struct libusb_device_handle *devh = libusb_open_device_with_vid_pid(nullptr, 0x1edb, 0xbd3b);
	//struct libusb_device_handle *devh = libusb_open_device_with_vid_pid(nullptr, 0x1edb, 0xbd4f);
	struct libusb_device_handle *devh = libusb_open_device_with_vid_pid(nullptr, vid, pid);
	if (!devh) {
		fprintf(stderr, "Error finding USB device\n");
		exit(1);
	}

	libusb_config_descriptor *config;
	rc = libusb_get_config_descriptor(libusb_get_device(devh), /*config_index=*/0, &config);
	if (rc < 0) {
		fprintf(stderr, "Error getting configuration: %s\n", libusb_error_name(rc));
		exit(1);
	}
	printf("%d interface\n", config->bNumInterfaces);
	for (int interface_number = 0; interface_number < config->bNumInterfaces; ++interface_number) {
		printf("  interface %d\n", interface_number);
		const libusb_interface *interface = &config->interface[interface_number];
		for (int altsetting = 0; altsetting < interface->num_altsetting; ++altsetting) {
			const libusb_interface_descriptor *interface_desc = &interface->altsetting[altsetting];
			printf("    alternate setting %d\n", interface_desc->bAlternateSetting);
			for (int endpoint_number = 0; endpoint_number < interface_desc->bNumEndpoints; ++endpoint_number) {
				const libusb_endpoint_descriptor *endpoint = &interface_desc->endpoint[endpoint_number];
				printf("        endpoint address 0x%02x\n", endpoint->bEndpointAddress);
			}
		}
	}

#if 0
	rc = libusb_set_configuration(devh, /*configuration=*/1);
	if (rc < 0) {
		fprintf(stderr, "Error setting configuration 1: %s\n", libusb_error_name(rc));
		exit(1);
	}
#endif

	rc = libusb_claim_interface(devh, 0);
	if (rc < 0) {
		fprintf(stderr, "Error claiming interface 0: %s\n", libusb_error_name(rc));
		exit(1);
	}

#if 0
	rc = libusb_set_interface_alt_setting(devh, /*interface=*/0, /*alternate_setting=*/1);
	if (rc < 0) {
		fprintf(stderr, "Error setting alternate 1: %s\n", libusb_error_name(rc));
		exit(1);
	}
#endif

#if 0
	rc = libusb_claim_interface(devh, 3);
	if (rc < 0) {
		fprintf(stderr, "Error claiming interface 3: %s\n", libusb_error_name(rc));
		exit(1);
	}
#endif

	// theories:
	//   44 is some kind of timer register (first 16 bits count upwards)
	//   24 is some sort of watchdog?
	//      you can seemingly set it to 0x73c60001 and that bit will eventually disappear
	//      (or will go to 0x73c60010?), also seen 0x73c60100
	//   12 also changes all the time, unclear why	
	//   16 seems to be autodetected mode somehow
	//      --    this is e00115e0 after reset?
	//                    ed0115e0 after mode change [to output?]
	//                    2d0015e0 after more mode change [to input]
	//                    ed0115e0 after more mode change
	//                    2d0015e0 after more mode change
	//
	//                    390115e0 seems to indicate we have signal
	//         changes to 200115e0 when resolution changes/we lose signal, driver resets after a while
	//
	//                    200015e0 on startup
	//         changes to 250115e0 when we sync to the signal
	//
	//    so only first 16 bits count, and 0x0100 is a mask for ok/stable signal?
	//
	//    28 and 32 seems to be analog audio input levels (one byte for each of the eight channels).
	//    however, if setting 32 with HDMI embedded audio, it is immediately overwritten back (to 0xe137002a).
	//
	//    4, 8, 20 are unclear. seem to be some sort of bitmask, but we can set them to 0 with no apparent effect.
	//    perhaps some of them are related to analog output?
	//
	//    36 can be set to 0 with no apparent effect (all of this tested on both video and audio),
	//    but the driver sets it to 0x8036802a at some point.
	//
	// register 16:
 	// first byte is 0x39 for a stable 576p60 signal, 0x2d for a stable 720p60 signal, 0x20 for no signal
	//
	// theories:
	//   0x01 - stable signal
	//   0x04 - deep color
	//   0x08 - unknown (audio??)
	//   0x20 - 720p??
	//   0x30 - 576p??

	struct ctrl {
		int endpoint;
		int request;
		int index;
		uint32_t data;
	};
	static const ctrl ctrls[] = {
		{ LIBUSB_ENDPOINT_IN,  214, 16, 0 },
		{ LIBUSB_ENDPOINT_IN,  214,  0, 0 },

		// seems to capture on HDMI, clearing the 0x20000000 bit seems to activate 10-bit
		// capture (v210).
		// clearing the 0x08000000 bit seems to change the capture format (other source?)
		// 0x10000000 = analog audio instead of embedded audio, it seems
		// 0x3a000000 = component video? (analog audio)
		// 0x3c000000 = composite video? (analog audio)
		// 0x3e000000 = s-video? (analog audio)
		{ LIBUSB_ENDPOINT_OUT, 215,  0, 0x29000000 },
		//{ LIBUSB_ENDPOINT_OUT, 215,  0, 0x80000100 },
		//{ LIBUSB_ENDPOINT_OUT, 215,  0, 0x09000000 },
		{ LIBUSB_ENDPOINT_OUT, 215, 24, 0x73c60001 },  // latch for frame start?
		{ LIBUSB_ENDPOINT_IN,  214, 24, 0 },  // 
	};

	for (unsigned req = 0; req < sizeof(ctrls) / sizeof(ctrls[0]); ++req) {
		uint32_t flipped = htonl(ctrls[req].data);
		static uint8_t value[4];
		memcpy(value, &flipped, sizeof(flipped));
		int size = sizeof(value);
		//if (ctrls[req].request == 215) size = 0;
		rc = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR | ctrls[req].endpoint,
			/*request=*/ctrls[req].request, /*value=*/0, /*index=*/ctrls[req].index, value, size, /*timeout=*/0);
		if (rc < 0) {
			fprintf(stderr, "Error on control %d: %s\n", ctrls[req].index, libusb_error_name(rc));
			exit(1);
		}
		
		printf("rc=%d: ep=%d@%d %d -> 0x", rc, ctrls[req].endpoint, ctrls[req].request, ctrls[req].index);
		for (int i = 0; i < rc; ++i) {
			printf("%02x", value[i]);
		}
		printf("\n");
	}

	// Alternate setting 1 is output, alternate setting 2 is input.
	// Card is reset when switching alternates, so the driver uses
	// this “double switch” when it wants to reset.
#if 0
	rc = libusb_set_interface_alt_setting(devh, /*interface=*/0, /*alternate_setting=*/1);
	if (rc < 0) {
		fprintf(stderr, "Error setting alternate 1: %s\n", libusb_error_name(rc));
		exit(1);
	}
#endif
	rc = libusb_set_interface_alt_setting(devh, /*interface=*/0, /*alternate_setting=*/4);
	if (rc < 0) {
		fprintf(stderr, "Error setting alternate 4: %s\n", libusb_error_name(rc));
		exit(1);
	}

#if 0
	// DEBUG
	for ( ;; ) {
		static int my_index = 0;
		static uint8_t value[4];
		int size = sizeof(value);
		rc = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN,
			/*request=*/214, /*value=*/0, /*index=*/my_index, value, size, /*timeout=*/0);
		if (rc < 0) {
			fprintf(stderr, "Error on control\n");
			exit(1);
		}
		printf("rc=%d index=%d: 0x", rc, my_index);
		for (int i = 0; i < rc; ++i) {
			printf("%02x", value[i]);
		}
		printf("\n");
	}
#endif

#if 0
	// set up an asynchronous transfer of the timer register
	static uint8_t cmdbuf[LIBUSB_CONTROL_SETUP_SIZE + 4];
	static int completed = 0;

	xfr = libusb_alloc_transfer(0);
	libusb_fill_control_setup(cmdbuf,
	    LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN, /*request=*/214, /*value=*/0,
		/*index=*/44, /*length=*/4);
	libusb_fill_control_transfer(xfr, devh, cmdbuf, cb_xfr, &completed, 0);
	xfr->user_data = this;
	libusb_submit_transfer(xfr);

	// set up an asynchronous transfer of register 24
	static uint8_t cmdbuf2[LIBUSB_CONTROL_SETUP_SIZE + 4];
	static int completed2 = 0;

	xfr = libusb_alloc_transfer(0);
	libusb_fill_control_setup(cmdbuf2,
	    LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN, /*request=*/214, /*value=*/0,
		/*index=*/24, /*length=*/4);
	libusb_fill_control_transfer(xfr, devh, cmdbuf2, cb_xfr, &completed2, 0);
	xfr->user_data = this;
	libusb_submit_transfer(xfr);
#endif

	// set up an asynchronous transfer of the register dump
	static uint8_t cmdbuf3[LIBUSB_CONTROL_SETUP_SIZE + 4];
	static int completed3 = 0;

	xfr = libusb_alloc_transfer(0);
	libusb_fill_control_setup(cmdbuf3,
	    LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN, /*request=*/214, /*value=*/0,
		/*index=*/current_register, /*length=*/4);
	libusb_fill_control_transfer(xfr, devh, cmdbuf3, cb_xfr, &completed3, 0);
	xfr->user_data = this;
	//libusb_submit_transfer(xfr);

	audiofp = fopen("audio.raw", "wb");

	// set up isochronous transfers for audio and video
	for (int e = 3; e <= 4; ++e) {
		//int num_transfers = (e == 3) ? 6 : 6;
		int num_transfers = 2;
		for (int i = 0; i < num_transfers; ++i) {
			int num_iso_pack, size;
			if (e == 3) {
				// Video seems to require isochronous packets scaled with the width; 
				// seemingly six lines is about right, rounded up to the required 1kB
				// multiple.
				size = WIDTH * 2 * 6;
				// Note that for 10-bit input, you'll need to increase size accordingly.
				//size = size * 4 / 3;
				if (size % 1024 != 0) {
					size &= ~1023;
					size += 1024;
				}
				num_iso_pack = (2 << 20) / size;  // 512 kB.
				printf("Picking %d packets of 0x%x bytes each\n", num_iso_pack, size);
			} else {
				size = 0xc0;
				num_iso_pack = 80;
			}
			int num_bytes = num_iso_pack * size;
			uint8_t *buf = new uint8_t[num_bytes];

			xfr = libusb_alloc_transfer(num_iso_pack);
			if (!xfr) {
				fprintf(stderr, "oom\n");
				exit(1);
			}

			int ep = LIBUSB_ENDPOINT_IN | e;
			libusb_fill_iso_transfer(xfr, devh, ep, buf, num_bytes,
				num_iso_pack, cb_xfr, nullptr, 0);
			libusb_set_iso_packet_lengths(xfr, size);
			xfr->user_data = this;
			iso_xfrs.push_back(xfr);
		}
	}
}

void BMUSBCapture::start_bm_capture()
{
	printf("starting capture\n");
	int i = 0;
	for (libusb_transfer *xfr : iso_xfrs) {
		printf("submitting transfer...\n");
		int rc = libusb_submit_transfer(xfr);
		++i;
		if (rc < 0) {
			//printf("num_bytes=%d\n", num_bytes);
			fprintf(stderr, "Error submitting iso to endpoint 0x%02x, number %d: %s\n",
				xfr->endpoint, i, libusb_error_name(rc));
			exit(1);
		}
	}


#if 0
	libusb_release_interface(devh, 0);
out:
	if (devh)
		libusb_close(devh);
	libusb_exit(nullptr);
	return rc;
#endif
}

void BMUSBCapture::start_bm_thread()
{
	should_quit = false;
	usb_thread = thread(&BMUSBCapture::usb_thread_func);
}

void BMUSBCapture::stop_bm_thread()
{
	should_quit = true;
	usb_thread.join();
}
