#include "decklink_capture.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cstddef>
#ifdef __SSE2__
#include <immintrin.h>
#endif

#include <DeckLinkAPI.h>
#include <DeckLinkAPIConfiguration.h>
#include <DeckLinkAPIDiscovery.h>
#include <DeckLinkAPIModes.h>
#include "bmusb/bmusb.h"

#define FRAME_SIZE (8 << 20)  // 8 MB.

using namespace std;
using namespace std::placeholders;

namespace {

// TODO: Support stride.
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

#ifdef __SSE2__

// Returns the number of bytes consumed.
size_t memcpy_interleaved_fastpath(uint8_t *dest1, uint8_t *dest2, const uint8_t *src, size_t n)
{
	const uint8_t *limit = src + n;
	size_t consumed = 0;

	// Align end to 32 bytes.
	limit = (const uint8_t *)(intptr_t(limit) & ~31);

	if (src >= limit) {
		return 0;
	}

	// Process [0,31] bytes, such that start gets aligned to 32 bytes.
	const uint8_t *aligned_src = (const uint8_t *)(intptr_t(src + 31) & ~31);
	if (aligned_src != src) {
		size_t n2 = aligned_src - src;
		memcpy_interleaved(dest1, dest2, src, n2);
		dest1 += n2 / 2;
		dest2 += n2 / 2;
		if (n2 % 1) {
			swap(dest1, dest2);
		}
		src = aligned_src;
		consumed += n2;
	}

	// Make the length a multiple of 64.
	if (((limit - src) % 64) != 0) {
		limit -= 32;
	}
	assert(((limit - src) % 64) == 0);

#if __AVX2__
	const __restrict __m256i *in = (const __m256i *)src;
	__restrict __m256i *out1 = (__m256i *)dest1;
	__restrict __m256i *out2 = (__m256i *)dest2;

	__m256i shuffle_cw = _mm256_set_epi8(
		15, 13, 11, 9, 7, 5, 3, 1, 14, 12, 10, 8, 6, 4, 2, 0,
		15, 13, 11, 9, 7, 5, 3, 1, 14, 12, 10, 8, 6, 4, 2, 0);
	while (in < (const __m256i *)limit) {
		// Note: For brevity, comments show lanes as if they were 2x64-bit (they're actually 2x128).
		__m256i data1 = _mm256_stream_load_si256(in);         // AaBbCcDd EeFfGgHh
		__m256i data2 = _mm256_stream_load_si256(in + 1);     // IiJjKkLl MmNnOoPp

		data1 = _mm256_shuffle_epi8(data1, shuffle_cw);       // ABCDabcd EFGHefgh
		data2 = _mm256_shuffle_epi8(data2, shuffle_cw);       // IJKLijkl MNOPmnop
	
		data1 = _mm256_permute4x64_epi64(data1, 0b11011000);  // ABCDEFGH abcdefgh
		data2 = _mm256_permute4x64_epi64(data2, 0b11011000);  // IJKLMNOP ijklmnop

		__m256i lo = _mm256_permute2x128_si256(data1, data2, 0b00100000);
		__m256i hi = _mm256_permute2x128_si256(data1, data2, 0b00110001);

		_mm256_storeu_si256(out1, lo);
		_mm256_storeu_si256(out2, hi);

		in += 2;
		++out1;
		++out2;
		consumed += 64;
	}
#else
	const __restrict __m128i *in = (const __m128i *)src;
	__restrict __m128i *out1 = (__m128i *)dest1;
	__restrict __m128i *out2 = (__m128i *)dest2;

	__m128i mask_lower_byte = _mm_set1_epi16(0x00ff);
	while (in < (const __m128i *)limit) {
		__m128i data1 = _mm_load_si128(in);
		__m128i data2 = _mm_load_si128(in + 1);
		__m128i data1_lo = _mm_and_si128(data1, mask_lower_byte);
		__m128i data2_lo = _mm_and_si128(data2, mask_lower_byte);
		__m128i data1_hi = _mm_srli_epi16(data1, 8);
		__m128i data2_hi = _mm_srli_epi16(data2, 8);
		__m128i lo = _mm_packus_epi16(data1_lo, data2_lo);
		_mm_storeu_si128(out1, lo);
		__m128i hi = _mm_packus_epi16(data1_hi, data2_hi);
		_mm_storeu_si128(out2, hi);

		in += 2;
		++out1;
		++out2;
		consumed += 32;
	}
#endif

	return consumed;
}

#endif  // __SSE2__

}  // namespace

DeckLinkCapture::DeckLinkCapture(IDeckLink *card, int card_index)
{
	{
		const char *model_name;
		char buf[256];
		if (card->GetModelName(&model_name) == S_OK) {
			snprintf(buf, sizeof(buf), "Card %d: %s", card_index, model_name);
		} else {
			snprintf(buf, sizeof(buf), "Card %d: Unknown DeckLink card", card_index);
		}
		description = buf;
	}

	if (card->QueryInterface(IID_IDeckLinkInput, (void**)&input) != S_OK) {
		fprintf(stderr, "Card %d has no inputs\n", card_index);
		exit(1);
	}

	/* Set up the video and audio sources. */
	IDeckLinkConfiguration *config;
	if (card->QueryInterface(IID_IDeckLinkConfiguration, (void**)&config) != S_OK) {
		fprintf(stderr, "Failed to get configuration interface for card %d\n", card_index);
		exit(1);
	}

	if (config->SetInt(bmdDeckLinkConfigVideoInputConnection, bmdVideoConnectionHDMI) != S_OK) {
		fprintf(stderr, "Failed to set video input connection for card %d\n", card_index);
		exit(1);
	}

	if (config->SetInt(bmdDeckLinkConfigAudioInputConnection, bmdAudioConnectionEmbedded) != S_OK) {
		fprintf(stderr, "Failed to set video input connection for card %d\n", card_index);
		exit(1);
	}

	// TODO: Make the user mode selectable.
	BMDDisplayModeSupport support;
	IDeckLinkDisplayMode *display_mode;
	if (input->DoesSupportVideoMode(bmdModeHD720p5994, bmdFormat8BitYUV, /*flags=*/0, &support, &display_mode)) {
		fprintf(stderr, "Failed to query display mode for card %d\n", card_index);
		exit(1);
	}

	if (support == bmdDisplayModeNotSupported) {
		fprintf(stderr, "Card %d does not support display mode\n", card_index);
		exit(1);
	}

	if (display_mode->GetFrameRate(&frame_duration, &time_scale) != S_OK) {
		fprintf(stderr, "Could not get frame rate for card %d\n", card_index);
		exit(1);
	}

	if (input->EnableVideoInput(bmdModeHD720p5994, bmdFormat8BitYUV, 0) != S_OK) {
		fprintf(stderr, "Failed to set 720p59.94 connection for card %d\n", card_index);
		exit(1);
	}

	if (input->EnableAudioInput(48000, bmdAudioSampleType32bitInteger, 2) != S_OK) {
		fprintf(stderr, "Failed to enable audio input for card %d\n", card_index);
		exit(1);
	}

	input->SetCallback(this);
}

DeckLinkCapture::~DeckLinkCapture()
{
	if (has_dequeue_callbacks) {
		dequeue_cleanup_callback();
	}
}

HRESULT STDMETHODCALLTYPE DeckLinkCapture::QueryInterface(REFIID, LPVOID *)
{
	return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE DeckLinkCapture::AddRef(void)
{
	return refcount.fetch_add(1) + 1;
}

ULONG STDMETHODCALLTYPE DeckLinkCapture::Release(void)
{
	int new_ref = refcount.fetch_sub(1) - 1;
	if (new_ref == 0)
		delete this;
	return new_ref;
}

HRESULT STDMETHODCALLTYPE DeckLinkCapture::VideoInputFormatChanged(
	BMDVideoInputFormatChangedEvents,
	IDeckLinkDisplayMode* display_mode,
	BMDDetectedVideoInputFormatFlags)
{
	if (display_mode->GetFrameRate(&frame_duration, &time_scale) != S_OK) {
		fprintf(stderr, "Could not get new frame rate\n");
		exit(1);
	}
	return S_OK;
}

HRESULT STDMETHODCALLTYPE DeckLinkCapture::VideoInputFrameArrived(
	IDeckLinkVideoInputFrame *video_frame,
	IDeckLinkAudioInputPacket *audio_frame)
{
	if (!done_init) {
		if (has_dequeue_callbacks) {
			dequeue_init_callback();
		}
		done_init = true;
	}

	FrameAllocator::Frame current_video_frame, current_audio_frame;
	VideoFormat video_format;
	AudioFormat audio_format;

	video_format.frame_rate_nom = time_scale;
	video_format.frame_rate_den = frame_duration;

	if (video_frame) {
		video_format.has_signal = !(video_frame->GetFlags() & bmdFrameHasNoInputSource);

		int width = video_frame->GetWidth();
		int height = video_frame->GetHeight();
		const int stride = video_frame->GetRowBytes();
		assert(stride == width * 2);

		current_video_frame = video_frame_allocator->alloc_frame();
		if (current_video_frame.data != nullptr) {
			const uint8_t *frame_bytes;
			video_frame->GetBytes((void **)&frame_bytes);

			uint8_t *data = current_video_frame.data;
			uint8_t *data2 = current_video_frame.data2;
			size_t num_bytes = width * height * 2;
#ifdef __SSE2__
			size_t consumed = memcpy_interleaved_fastpath(data, data2, frame_bytes, num_bytes);
			frame_bytes += consumed;
			data += consumed / 2;
			data2 += consumed / 2;
			if (num_bytes % 2) {
				swap(data, data2);
			}
			current_video_frame.len += consumed;
			num_bytes -= consumed;
#endif

			if (num_bytes > 0) {
				memcpy_interleaved(data, data2, frame_bytes, num_bytes);
			}
			current_video_frame.len += num_bytes;

			video_format.width = width;
			video_format.height = height;
		}
	}

	if (audio_frame) {
		int num_samples = audio_frame->GetSampleFrameCount();

		current_audio_frame = audio_frame_allocator->alloc_frame();
		if (current_audio_frame.data != nullptr) {
			const uint8_t *frame_bytes;
			audio_frame->GetBytes((void **)&frame_bytes);
			current_audio_frame.len = sizeof(int32_t) * 2 * num_samples;

			memcpy(current_audio_frame.data, frame_bytes, current_audio_frame.len);

			audio_format.bits_per_sample = 32;
			audio_format.num_channels = 2;
		}
	}

	if (current_video_frame.data != nullptr || current_audio_frame.data != nullptr) {
		// TODO: Put into a queue and put into a dequeue thread, if the
		// BlackMagic drivers don't already do that for us?
		frame_callback(timecode,
			current_video_frame, /*video_offset=*/0, video_format,
			current_audio_frame, /*audio_offset=*/0, audio_format);
	}

	timecode++;
	return S_OK;
}

void DeckLinkCapture::configure_card()
{
	if (video_frame_allocator == nullptr) {
		set_video_frame_allocator(new MallocFrameAllocator(FRAME_SIZE, NUM_QUEUED_VIDEO_FRAMES));  // FIXME: leak.
	}
	if (audio_frame_allocator == nullptr) {
		set_audio_frame_allocator(new MallocFrameAllocator(65536, NUM_QUEUED_AUDIO_FRAMES));  // FIXME: leak.
	}
}

void DeckLinkCapture::start_bm_capture()
{
	if (input->StartStreams() != S_OK) {
		fprintf(stderr, "StartStreams failed\n");
		exit(1);
	}
}

void DeckLinkCapture::stop_dequeue_thread()
{
	if (input->StopStreams() != S_OK) {
		fprintf(stderr, "StopStreams failed\n");
		exit(1);
	}
}

