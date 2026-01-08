#pragma once

#include <ytdlpp/ytdlpp_export.h>

#include <boost/asio/any_completion_executor.hpp>
#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/buffer.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "result.hpp"

namespace ytdlpp::media {

namespace asio = boost::asio;

// Forward declaration
class AudioStream;

/// Sample formats matching FFmpeg's AVSampleFormat values
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

/// Audio stream configuration
struct YTDLPP_EXPORT AudioStreamOptions {
	int sample_rate = 48000;  // Common rates: 44100, 48000, 96000
	int channels = 2;		  // 1 = mono, 2 = stereo
	SampleFormat sample_fmt = SampleFormat::S16;
};

/// AudioStream - an async-readable audio stream backed by FFmpeg decoding.
///
/// This class provides an asio-compatible async read interface for decoded
/// audio data. The decoding runs in a background thread and data is buffered
/// for async consumption.
///
/// Example usage with yield_context:
///   AudioStream stream = streamer.async_open(url, opts, yield).value();
///   while (!stream.is_eof()) {
///       auto result = stream.async_read(yield);
///       // process result.value() bytes
///   }
class YTDLPP_EXPORT AudioStream {
   public:
	AudioStream(AudioStream &&) noexcept;
	AudioStream &operator=(AudioStream &&) noexcept;
	~AudioStream();

	// Non-copyable
	AudioStream(const AudioStream &) = delete;
	AudioStream &operator=(const AudioStream &) = delete;

	/// Check if stream has reached end of file
	[[nodiscard]] bool is_eof() const;

	/// Check if stream was cancelled
	[[nodiscard]] bool is_cancelled() const;

	/// Cancel the stream (thread-safe)
	void cancel();

	/// Get the executor
	[[nodiscard]] asio::any_io_executor get_executor() const;

	using CompletionExecutor = asio::any_completion_executor;

	/// Async read decoded PCM data into buffer.
	/// Returns number of bytes read, or 0 on EOF.
	/// Signature: void(Result<size_t>)
	template <BOOST_ASIO_COMPLETION_TOKEN_FOR(void(Result<size_t>))
				  CompletionToken>
	auto async_read(asio::mutable_buffer buffer, CompletionToken &&token) {
		auto ex = get_executor();
		return asio::async_initiate<CompletionToken, void(Result<size_t>)>(
			[this, ex, buffer](auto &&handler) mutable {
				CompletionExecutor handler_ex =
					asio::get_associated_executor(handler, ex);

				auto slot = asio::get_associated_cancellation_slot(handler);

				auto any_handler =
					asio::any_completion_handler<void(Result<size_t>)>{
						std::forward<decltype(handler)>(handler)};

				async_read_impl(buffer, std::move(any_handler),
								std::move(handler_ex), slot);
			},
			token);
	}

	/// Async read that allocates the buffer for you.
	/// Returns empty vector on EOF.
	/// Signature: void(Result<std::vector<uint8_t>>)
	template <BOOST_ASIO_COMPLETION_TOKEN_FOR(
		void(Result<std::vector<uint8_t>>)) CompletionToken>
	auto async_read(CompletionToken &&token) {
		auto ex = get_executor();
		return asio::async_initiate<CompletionToken,
									void(Result<std::vector<uint8_t>>)>(
			[this, ex](auto &&handler) mutable {
				CompletionExecutor handler_ex =
					asio::get_associated_executor(handler, ex);

				auto slot = asio::get_associated_cancellation_slot(handler);

				auto any_handler = asio::any_completion_handler<void(
					Result<std::vector<uint8_t>>)>{
					std::forward<decltype(handler)>(handler)};

				async_read_alloc_impl(
					std::move(any_handler), std::move(handler_ex), slot);
			},
			token);
	}

   private:
	friend class AudioStreamer;

	struct Impl;
	explicit AudioStream(std::shared_ptr<Impl> impl);

	void async_read_impl(
		asio::mutable_buffer buffer,
		asio::any_completion_handler<void(Result<size_t>)> handler,
		CompletionExecutor handler_ex, asio::cancellation_slot slot);

	void async_read_alloc_impl(
		asio::any_completion_handler<void(Result<std::vector<uint8_t>>)>
			handler,
		CompletionExecutor handler_ex, asio::cancellation_slot slot);

	std::shared_ptr<Impl> m_impl;
};

/// AudioStreamer - factory for creating AudioStream instances.
///
/// Opens audio streams from URLs using FFmpeg for decoding.
class YTDLPP_EXPORT AudioStreamer {
   public:
	AudioStreamer(const AudioStreamer &) = delete;
	AudioStreamer &operator=(const AudioStreamer &) = delete;
	AudioStreamer(AudioStreamer &&) noexcept;
	AudioStreamer &operator=(AudioStreamer &&) noexcept;
	~AudioStreamer();

	explicit AudioStreamer(asio::any_io_executor ex);

	[[nodiscard]] asio::any_io_executor get_executor() const;

	using CompletionExecutor = asio::any_completion_executor;

	/// Open an audio stream asynchronously.
	/// Returns an AudioStream that can be read from.
	/// Signature: void(Result<AudioStream>)
	template <BOOST_ASIO_COMPLETION_TOKEN_FOR(void(Result<AudioStream>))
				  CompletionToken>
	auto async_open(std::string_view url, const AudioStreamOptions &options,
					CompletionToken &&token) {
		auto ex = get_executor();
		return asio::async_initiate<CompletionToken, void(Result<AudioStream>)>(
			[this, ex, url_s = std::string(url),
			 options](auto &&handler) mutable {
				CompletionExecutor handler_ex =
					asio::get_associated_executor(handler, ex);

				auto slot = asio::get_associated_cancellation_slot(handler);

				auto any_handler =
					asio::any_completion_handler<void(Result<AudioStream>)>{
						std::forward<decltype(handler)>(handler)};

				async_open_impl(
					std::move(url_s), options, std::move(any_handler),
					std::move(handler_ex), slot);
			},
			token);
	}

   private:
	struct Impl;

	void async_open_impl(
		std::string url, AudioStreamOptions options,
		asio::any_completion_handler<void(Result<AudioStream>)> handler,
		CompletionExecutor handler_ex, asio::cancellation_slot slot);

	std::unique_ptr<Impl> m_impl;
};

}  // namespace ytdlpp::media
