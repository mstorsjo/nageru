#ifndef _MUX_H
#define _MUX_H 1

// Wrapper around an AVFormat mux.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
}

class PacketDestination {
public:
	virtual ~PacketDestination() {}
	virtual void add_packet(const AVPacket &pkt, int64_t pts, int64_t dts) = 0;
};

class Mux : public PacketDestination {
public:
	enum Codec {
		CODEC_H264,
		CODEC_NV12,  // Uncompressed 4:2:0.
	};

	// Takes ownership of avctx.
	Mux(AVFormatContext *avctx, int width, int height, Codec video_codec, int time_base, int bit_rate);
	~Mux();
	void add_packet(const AVPacket &pkt, int64_t pts, int64_t dts) override;

private:
	bool seen_keyframe = false;
	AVFormatContext *avctx;
	AVStream *avstream_video, *avstream_audio;
};

#endif  // !defined(_MUX_H)
