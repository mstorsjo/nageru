#ifndef _MUX_H
#define _MUX_H 1

// Wrapper around an AVFormat mux.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
}

#include <mutex>

class KeyFrameSignalReceiver {
public:
	// Needs to automatically turn the flag off again after actually receiving data.
	virtual void signal_keyframe() = 0;
};

class Mux {
public:
	enum Codec {
		CODEC_H264,
		CODEC_NV12,  // Uncompressed 4:2:0.
	};

	// Takes ownership of avctx. <keyframe_signal_receiver> can be nullptr.
	Mux(AVFormatContext *avctx, int width, int height, Codec video_codec, const AVCodec *codec_audio, int time_base, int bit_rate, KeyFrameSignalReceiver *keyframe_signal_receiver);
	~Mux();
	void add_packet(const AVPacket &pkt, int64_t pts, int64_t dts);

private:
	std::mutex ctx_mu;
	AVFormatContext *avctx;  // Protected by <ctx_mu>.
	AVStream *avstream_video, *avstream_audio;
	KeyFrameSignalReceiver *keyframe_signal_receiver;
};

#endif  // !defined(_MUX_H)
