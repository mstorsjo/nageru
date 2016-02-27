#ifndef _DECKLINK_CAPTURE_H
#define _DECKLINK_CAPTURE_H 1

#include <DeckLinkAPI.h>
#include <stdint.h>
#include <atomic>
#include <functional>
#include <string>

#include "bmusb/bmusb.h"

class IDeckLink;
class IDeckLinkDisplayMode;

// TODO: Adjust CaptureInterface to be a little less bmusb-centric.
// There are too many member functions here that don't really do anything.
class DeckLinkCapture : public CaptureInterface, IDeckLinkInputCallback
{
public:
	DeckLinkCapture(IDeckLink *card, int card_index);
	~DeckLinkCapture();

	// IDeckLinkInputCallback.
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, LPVOID *) override;
	ULONG STDMETHODCALLTYPE AddRef() override;
	ULONG STDMETHODCALLTYPE Release() override;
	HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(
		BMDVideoInputFormatChangedEvents,
		IDeckLinkDisplayMode*,
		BMDDetectedVideoInputFormatFlags) override;
	HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(
		IDeckLinkVideoInputFrame *video_frame,
		IDeckLinkAudioInputPacket *audio_frame) override;

	// CaptureInterface.
	void set_video_frame_allocator(FrameAllocator *allocator) override
	{
		video_frame_allocator = allocator;
	}

	FrameAllocator *get_video_frame_allocator() override
	{
		return video_frame_allocator;
	}

	// Does not take ownership.
	void set_audio_frame_allocator(FrameAllocator *allocator) override
	{
		audio_frame_allocator = allocator;
	}

	FrameAllocator *get_audio_frame_allocator() override
	{
		return audio_frame_allocator;
	}

	void set_frame_callback(frame_callback_t callback) override
	{
		frame_callback = callback;
	}

	void set_dequeue_thread_callbacks(std::function<void()> init, std::function<void()> cleanup) override
	{
		dequeue_init_callback = init;
		dequeue_cleanup_callback = cleanup;
		has_dequeue_callbacks = true;
	}

	std::string get_description() const override
	{
		return description;
	}

	void configure_card() override;
	void start_bm_capture() override;
	void stop_dequeue_thread() override;

private:
	std::atomic<int> refcount{1};
	bool done_init = false;
	std::string description;
	uint16_t timecode = 0;

	bool has_dequeue_callbacks = false;
	std::function<void()> dequeue_init_callback = nullptr;
	std::function<void()> dequeue_cleanup_callback = nullptr;

	FrameAllocator *video_frame_allocator = nullptr;
	FrameAllocator *audio_frame_allocator = nullptr;
	frame_callback_t frame_callback = nullptr;

	IDeckLinkInput *input = nullptr;
	BMDTimeValue frame_duration;
	BMDTimeScale time_scale;
};

#endif  // !defined(_DECKLINK_CAPTURE_H)
