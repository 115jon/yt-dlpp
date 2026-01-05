#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

}

#include <memory>

namespace ytdlpp::media {

struct AVFormatContextDeleter {
	void operator()(AVFormatContext *ctx) const {
		if (ctx) {
			// For input context, use close_input
			// For output context (if opened via avio_open), use free_context +
			// avio_close? Actually standard practice calls for differentiation.
			// We might just use simple wrappers in Muxer instead of generic
			// deletes if usage differs But let's try to be smart. If it's an
			// input context (oformat is NULL), close input. If it's output,
			// close output.

			// Wait, standard idiom:
			// Input: avformat_close_input(&ctx)
			// Output: avformat_free_context(ctx)

			// If we wrapp input and output separately it is cleaner.
			// But avformat_close_input takes **s.
		}
	}
};

// Simple RAII for AVPacket
struct AVPacketDeleter {
	void operator()(AVPacket *pkt) const {
		if (pkt) { av_packet_free(&pkt); }
	}
};

using AVPacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;

inline AVPacketPtr make_packet() { return AVPacketPtr(av_packet_alloc()); }

}  // namespace ytdlpp::media
