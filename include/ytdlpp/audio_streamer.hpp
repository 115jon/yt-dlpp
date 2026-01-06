#pragma once

#include <boost/asio/any_completion_executor.hpp>
#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/async_result.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "result.hpp"

namespace ytdlpp::media {

namespace asio = boost::asio;

class AudioStreamer {
   public:
	AudioStreamer(const AudioStreamer &) = delete;
	AudioStreamer &operator=(const AudioStreamer &) = delete;
	AudioStreamer(AudioStreamer &&) noexcept;
	AudioStreamer &operator=(AudioStreamer &&) noexcept;
	~AudioStreamer();

	explicit AudioStreamer(asio::any_io_executor ex);

	// Cancellation
	void cancel();

	[[nodiscard]] asio::any_io_executor get_executor() const;

	// Sample formats matching FFmpeg's AVSampleFormat values
	enum class SampleFormat : std::uint8_t {
		U8 = 0,		// unsigned 8 bits
		S16 = 1,	// signed 16 bits (default, most compatible)
		S32 = 2,	// signed 32 bits
		FLT = 3,	// float
		DBL = 4,	// double
		U8P = 5,	// unsigned 8 bits, planar
		S16P = 6,	// signed 16 bits, planar
		S32P = 7,	// signed 32 bits, planar
		FLTP = 8,	// float, planar
		DBLP = 9,	// double, planar
		S64 = 10,	// signed 64 bits
		S64P = 11,	// signed 64 bits, planar
	};

	struct AudioStreamOptions {
		int sample_rate = 48000;  // Common rates: 44100, 48000, 96000
		int channels = 2;		  // 1 = mono, 2 = stereo
		SampleFormat sample_fmt = SampleFormat::S16;
	};

	using DataCallback = std::function<void(const std::vector<uint8_t> &)>;
	using CompletionExecutor = asio::any_completion_executor;

	template <BOOST_ASIO_COMPLETION_TOKEN_FOR(void(Result<void>))
				  CompletionToken>
	auto async_stream(std::string_view url, const AudioStreamOptions &options,
					  DataCallback on_data, CompletionToken &&token) {
		auto ex = get_executor();
		return asio::async_initiate<CompletionToken, void(Result<void>)>(
			[this, ex, url_s = std::string(url), options,
			 on_data = std::move(on_data)](auto &&handler) mutable {
				CompletionExecutor handler_ex =
					asio::get_associated_executor(handler, ex);

				// Per-operation cancellation
				auto slot = asio::get_associated_cancellation_slot(handler);

				auto any_handler =
					asio::any_completion_handler<void(Result<void>)>{
						std::forward<decltype(handler)>(handler)};

				async_stream_impl(
					std::move(url_s), options, std::move(on_data),
					std::move(any_handler), std::move(handler_ex), slot);
			},
			token);
	}

   private:
	struct Impl;

	void async_stream_impl(
		std::string url, AudioStreamOptions options, DataCallback on_data,
		asio::any_completion_handler<void(Result<void>)> handler,
		CompletionExecutor handler_ex, asio::cancellation_slot slot);

	std::unique_ptr<Impl> m_impl;
};

}  // namespace ytdlpp::media
