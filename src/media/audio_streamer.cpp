#include <spdlog/spdlog.h>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/scope_exit.hpp>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <ytdlpp/audio_streamer.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace ytdlpp::media {

// =============================================================================
// AudioStream Implementation
// =============================================================================

struct AudioStream::Impl : std::enable_shared_from_this<AudioStream::Impl> {
	asio::any_io_executor ex;
	asio::strand<asio::any_io_executor> strand;

	// Thread-safe state
	std::mutex mutex;
	std::condition_variable cv;
	std::deque<std::vector<uint8_t>> buffer_queue;
	bool eof = false;
	bool cancelled = false;
	bool producer_done = false;

	// Pending read operation (with buffer)
	struct PendingRead {
		uint8_t *data;
		size_t size;
		asio::any_completion_handler<void(Result<size_t>)> handler;
		AudioStream::CompletionExecutor handler_ex;
	};
	std::optional<PendingRead> pending_read;

	// Pending read operation (allocating)
	struct PendingAllocRead {
		asio::any_completion_handler<void(Result<std::vector<uint8_t>>)>
			handler;
		AudioStream::CompletionExecutor handler_ex;
	};
	std::optional<PendingAllocRead> pending_alloc_read;

	explicit Impl(asio::any_io_executor e)
		: ex(std::move(e)), strand(asio::make_strand(ex)) {}

	~Impl() { cancel(); }

	void cancel() {
		std::unique_lock lock(mutex);
		cancelled = true;
		cv.notify_all();

		// Move pending reads out while holding lock
		std::optional<PendingRead> pr = std::move(pending_read);
		pending_read.reset();
		std::optional<PendingAllocRead> par = std::move(pending_alloc_read);
		pending_alloc_read.reset();

		// Release lock before completing handlers
		lock.unlock();

		// Complete any pending reads with cancellation
		if (pr) {
			asio::dispatch(asio::bind_executor(
				pr->handler_ex, [h = std::move(pr->handler)]() mutable {
					h(outcome::failure(asio::error::operation_aborted));
				}));
		}
		if (par) {
			asio::dispatch(asio::bind_executor(
				par->handler_ex, [h = std::move(par->handler)]() mutable {
					h(outcome::failure(asio::error::operation_aborted));
				}));
		}
	}

	// Called by producer thread when data is available
	void push_data(std::vector<uint8_t> data) {
		std::unique_lock lock(mutex);
		if (cancelled) return;

		buffer_queue.push_back(std::move(data));

		// If there's a pending read, complete it
		auto pr = std::move(pending_read);
		pending_read.reset();
		auto par = std::move(pending_alloc_read);
		pending_alloc_read.reset();

		if (pr || par) {
			// Need to complete a pending read
			if (pr) {
				auto &front = buffer_queue.front();
				size_t to_copy = std::min(pr->size, front.size());
				std::memcpy(pr->data, front.data(), to_copy);

				if (to_copy >= front.size()) {
					buffer_queue.pop_front();
				} else {
					front.erase(
						front.begin(),
						front.begin() + static_cast<std::ptrdiff_t>(to_copy));
				}

				lock.unlock();
				asio::dispatch(asio::bind_executor(
					pr->handler_ex,
					[h = std::move(pr->handler), to_copy]() mutable {
						h(outcome::success(to_copy));
					}));
			} else if (par) {
				auto result_data = std::move(buffer_queue.front());
				buffer_queue.pop_front();

				lock.unlock();
				asio::dispatch(asio::bind_executor(
					par->handler_ex, [h = std::move(par->handler),
									  d = std::move(result_data)]() mutable {
						h(outcome::success(std::move(d)));
					}));
			}
		}
	}

	// Called by producer thread when done
	void set_eof() {
		std::unique_lock lock(mutex);
		eof = true;
		producer_done = true;

		// Move out pending reads
		auto pr = std::move(pending_read);
		pending_read.reset();
		auto par = std::move(pending_alloc_read);
		pending_alloc_read.reset();

		lock.unlock();

		// Complete any pending read with EOF (0 bytes / empty vector)
		if (pr) {
			asio::dispatch(asio::bind_executor(
				pr->handler_ex, [h = std::move(pr->handler)]() mutable {
					h(outcome::success(size_t{0}));
				}));
		}
		if (par) {
			asio::dispatch(asio::bind_executor(
				par->handler_ex, [h = std::move(par->handler)]() mutable {
					h(outcome::success(std::vector<uint8_t>{}));
				}));
		}
	}
};

AudioStream::AudioStream(std::shared_ptr<Impl> impl)
	: m_impl(std::move(impl)) {}

AudioStream::AudioStream(AudioStream &&) noexcept = default;
AudioStream &AudioStream::operator=(AudioStream &&) noexcept = default;
AudioStream::~AudioStream() = default;

bool AudioStream::is_eof() const {
	if (!m_impl) return true;
	std::lock_guard lock(m_impl->mutex);
	return m_impl->eof && m_impl->buffer_queue.empty();
}

bool AudioStream::is_cancelled() const {
	if (!m_impl) return true;
	std::lock_guard lock(m_impl->mutex);
	return m_impl->cancelled;
}

void AudioStream::cancel() {
	if (m_impl) m_impl->cancel();
}

asio::any_io_executor AudioStream::get_executor() const { return m_impl->ex; }

void AudioStream::async_read_impl(
	asio::mutable_buffer buffer,
	asio::any_completion_handler<void(Result<size_t>)> handler,
	CompletionExecutor handler_ex, asio::cancellation_slot slot) {
	auto self = m_impl;

	// Bind cancellation
	if (slot.is_connected()) {
		slot.assign([weak = std::weak_ptr(self)](asio::cancellation_type type) {
			if (type != asio::cancellation_type::none) {
				if (auto s = weak.lock()) { s->cancel(); }
			}
		});
	}

	auto *data_ptr = static_cast<uint8_t *>(buffer.data());
	size_t data_size = buffer.size();

	std::unique_lock lock(self->mutex);

	// If data available or EOF, complete immediately
	if (!self->buffer_queue.empty() || self->eof || self->cancelled) {
		if (self->cancelled) {
			lock.unlock();
			asio::dispatch(asio::bind_executor(
				handler_ex, [handler = std::move(handler)]() mutable {
					handler(outcome::failure(asio::error::operation_aborted));
				}));
		} else if (self->buffer_queue.empty()) {
			lock.unlock();
			asio::dispatch(asio::bind_executor(
				handler_ex, [handler = std::move(handler)]() mutable {
					handler(outcome::success(size_t{0}));
				}));
		} else {
			auto &front = self->buffer_queue.front();
			size_t to_copy = std::min(data_size, front.size());
			std::memcpy(data_ptr, front.data(), to_copy);

			if (to_copy >= front.size()) {
				self->buffer_queue.pop_front();
			} else {
				front.erase(
					front.begin(),
					front.begin() + static_cast<std::ptrdiff_t>(to_copy));
			}

			lock.unlock();
			asio::dispatch(asio::bind_executor(
				handler_ex, [handler = std::move(handler), to_copy]() mutable {
					handler(outcome::success(to_copy));
				}));
		}
	} else {
		// Store pending read
		self->pending_read = Impl::PendingRead{
			data_ptr, data_size, std::move(handler), std::move(handler_ex)};
	}
}

void AudioStream::async_read_alloc_impl(
	asio::any_completion_handler<void(Result<std::vector<uint8_t>>)> handler,
	CompletionExecutor handler_ex, asio::cancellation_slot slot) {
	auto self = m_impl;

	if (slot.is_connected()) {
		slot.assign([weak = std::weak_ptr(self)](asio::cancellation_type type) {
			if (type != asio::cancellation_type::none) {
				if (auto s = weak.lock()) { s->cancel(); }
			}
		});
	}

	std::unique_lock lock(self->mutex);

	if (!self->buffer_queue.empty() || self->eof || self->cancelled) {
		if (self->cancelled) {
			lock.unlock();
			asio::dispatch(asio::bind_executor(
				handler_ex, [handler = std::move(handler)]() mutable {
					handler(outcome::failure(asio::error::operation_aborted));
				}));
		} else if (self->buffer_queue.empty()) {
			lock.unlock();
			asio::dispatch(asio::bind_executor(
				handler_ex, [handler = std::move(handler)]() mutable {
					handler(outcome::success(std::vector<uint8_t>{}));
				}));
		} else {
			auto data = std::move(self->buffer_queue.front());
			self->buffer_queue.pop_front();

			lock.unlock();
			asio::dispatch(asio::bind_executor(
				handler_ex, [handler = std::move(handler),
							 data = std::move(data)]() mutable {
					handler(outcome::success(std::move(data)));
				}));
		}
	} else {
		self->pending_alloc_read =
			Impl::PendingAllocRead{std::move(handler), std::move(handler_ex)};
	}
}

// =============================================================================
// AudioStreamer Implementation
// =============================================================================

// Interrupt callback for FFmpeg
static int interrupt_callback(void *ctx) {
	auto *cancel_token = static_cast<std::atomic<bool> *>(ctx);
	return cancel_token->load() ? 1 : 0;
}

struct AudioStreamer::Impl {
	asio::any_io_executor ex;
	asio::thread_pool work_pool{1};

	explicit Impl(asio::any_io_executor e) : ex(std::move(e)) {}

	~Impl() { work_pool.join(); }

	// Producer function - runs in thread pool
	static void stream_producer(
		const std::weak_ptr<AudioStream::Impl> &weak_impl,
		const std::string &url, AudioStreamOptions options,
		const std::shared_ptr<std::atomic<bool>> &cancel_token) {
		AVFormatContext *format_ctx = nullptr;
		AVCodecContext *codec_ctx = nullptr;
		SwrContext *swr_ctx = nullptr;
		AVPacket *packet = nullptr;
		AVFrame *frame = nullptr;

		BOOST_SCOPE_EXIT_ALL(
			&frame, &packet, &swr_ctx, &codec_ctx, &format_ctx) {
			if (frame) av_frame_free(&frame);
			if (packet) av_packet_free(&packet);
			if (swr_ctx) swr_free(&swr_ctx);
			if (codec_ctx) avcodec_free_context(&codec_ctx);
			if (format_ctx) avformat_close_input(&format_ctx);
		};

		auto mark_eof = [&]() {
			if (auto impl = weak_impl.lock()) { impl->set_eof(); }
		};

		// Allocate format context
		format_ctx = avformat_alloc_context();
		if (!format_ctx) {
			spdlog::error("AudioStream: Failed to allocate format context");
			mark_eof();
			return;
		}

		format_ctx->interrupt_callback.callback = interrupt_callback;
		format_ctx->interrupt_callback.opaque = cancel_token.get();

		// Network options
		{
			AVDictionary *av_options = nullptr;
			av_dict_set(&av_options, "reconnect", "1", 0);
			av_dict_set(&av_options, "reconnect_streamed", "1", 0);
			av_dict_set(&av_options, "reconnect_at_eof", "1", 0);
			av_dict_set(&av_options, "reconnect_delay_max", "5", 0);
			av_dict_set(
				&av_options, "user_agent",
				"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
				"(KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36",
				0);

			BOOST_SCOPE_EXIT_ALL(&av_options) { av_dict_free(&av_options); };

			if (avformat_open_input(
					&format_ctx, url.c_str(), nullptr, &av_options) < 0) {
				spdlog::error("AudioStream: Failed to open input URL");
				mark_eof();
				return;
			}
		}

		if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
			spdlog::error("AudioStream: Failed to find stream info");
			mark_eof();
			return;
		}

		int stream_idx = av_find_best_stream(
			format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
		if (stream_idx < 0) {
			spdlog::error("AudioStream: No audio stream found");
			mark_eof();
			return;
		}

		AVStream *stream = format_ctx->streams[stream_idx];
		const AVCodec *decoder =
			avcodec_find_decoder(stream->codecpar->codec_id);
		if (!decoder) {
			spdlog::error("AudioStream: Codec not found");
			mark_eof();
			return;
		}

		codec_ctx = avcodec_alloc_context3(decoder);
		avcodec_parameters_to_context(codec_ctx, stream->codecpar);

		if (avcodec_open2(codec_ctx, decoder, nullptr) < 0) {
			spdlog::error("AudioStream: Failed to open codec");
			mark_eof();
			return;
		}

		// Setup resampler
		int dst_rate = options.sample_rate;
		auto dst_sample_fmt = static_cast<AVSampleFormat>(options.sample_fmt);

		swr_ctx = swr_alloc();
		av_opt_set_chlayout(
			swr_ctx, "in_chlayout", &stream->codecpar->ch_layout, 0);
		av_opt_set_int(
			swr_ctx, "in_sample_rate", stream->codecpar->sample_rate, 0);
		av_opt_set_sample_fmt(
			swr_ctx, "in_sample_fmt",
			static_cast<AVSampleFormat>(stream->codecpar->format), 0);

		AVChannelLayout out_layout{};
		av_channel_layout_default(&out_layout, options.channels);
		av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_layout, 0);
		av_opt_set_int(swr_ctx, "out_sample_rate", dst_rate, 0);
		av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", dst_sample_fmt, 0);

		if (swr_init(swr_ctx) < 0) {
			spdlog::error("AudioStream: Failed to init resampler");
			mark_eof();
			return;
		}

		packet = av_packet_alloc();
		frame = av_frame_alloc();

		// Pre-allocate destination buffer
		const int initial_samples = 2048;
		uint8_t **dst_data = nullptr;
		int dst_linesize = 0;
		int allocated_samples = initial_samples;

		av_samples_alloc_array_and_samples(
			&dst_data, &dst_linesize, options.channels, allocated_samples,
			dst_sample_fmt, 0);

		BOOST_SCOPE_EXIT_ALL(&dst_data) {
			if (dst_data) {
				av_freep(&dst_data[0]);
				av_freep(&dst_data);
			}
		};

		// Decode loop
		while (!cancel_token->load(std::memory_order_relaxed)) {
			// Check if consumer is still alive
			auto impl = weak_impl.lock();
			if (!impl) break;

			int ret = av_read_frame(format_ctx, packet);
			if (ret < 0) break;

			if (packet->stream_index == stream_idx) {
				ret = avcodec_send_packet(codec_ctx, packet);
				if (ret >= 0) {
					while (ret >= 0 &&
						   !cancel_token->load(std::memory_order_relaxed)) {
						ret = avcodec_receive_frame(codec_ctx, frame);
						if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
						if (ret < 0) {
							spdlog::error("AudioStream: Error during decoding");
							break;
						}

						// Calculate output samples
						int max_dst_nb_samples =
							static_cast<int>(av_rescale_rnd(
								swr_get_delay(swr_ctx, codec_ctx->sample_rate) +
									frame->nb_samples,
								dst_rate, codec_ctx->sample_rate, AV_ROUND_UP));

						// Reallocate if needed
						if (max_dst_nb_samples > allocated_samples) {
							av_freep(&dst_data[0]);
							av_freep(&dst_data);
							allocated_samples = max_dst_nb_samples;
							av_samples_alloc_array_and_samples(
								&dst_data, &dst_linesize, options.channels,
								allocated_samples, dst_sample_fmt, 0);
						}

						int nb_samples = swr_convert(
							swr_ctx, dst_data, max_dst_nb_samples,
							const_cast<const uint8_t **>(frame->data),
							frame->nb_samples);

						if (nb_samples > 0) {
							int size = av_samples_get_buffer_size(
								nullptr, options.channels, nb_samples,
								dst_sample_fmt, 1);

							std::vector<uint8_t> buffer(
								static_cast<size_t>(size));
							std::memcpy(buffer.data(), dst_data[0],
										static_cast<size_t>(size));

							// Push to consumer
							if (auto i = weak_impl.lock()) {
								i->push_data(std::move(buffer));
							} else {
								break;	// Consumer gone
							}
						}
					}
				}
			}

			av_packet_unref(packet);
		}

		mark_eof();
	}
};

AudioStreamer::AudioStreamer(asio::any_io_executor ex)
	: m_impl(std::make_unique<Impl>(std::move(ex))) {}

AudioStreamer::AudioStreamer(AudioStreamer &&) noexcept = default;
AudioStreamer &AudioStreamer::operator=(AudioStreamer &&) noexcept = default;
AudioStreamer::~AudioStreamer() = default;

asio::any_io_executor AudioStreamer::get_executor() const { return m_impl->ex; }

void AudioStreamer::async_open_impl(
	std::string url, AudioStreamOptions options,
	asio::any_completion_handler<void(Result<AudioStream>)> handler,
	CompletionExecutor handler_ex, asio::cancellation_slot slot) {
	// Create the stream impl
	auto stream_impl = std::make_shared<AudioStream::Impl>(m_impl->ex);
	auto cancel_token = std::make_shared<std::atomic<bool>>(false);

	// Bind cancellation
	if (slot.is_connected()) {
		slot.assign([cancel_token, weak = std::weak_ptr(stream_impl)](
						asio::cancellation_type type) {
			if (type != asio::cancellation_type::none) {
				*cancel_token = true;
				if (auto s = weak.lock()) { s->cancel(); }
			}
		});
	}

	// Start producer thread
	std::weak_ptr<AudioStream::Impl> weak_impl = stream_impl;
	asio::post(m_impl->work_pool, [weak_impl, url = std::move(url), options,
								   cancel_token]() mutable {
		Impl::stream_producer(weak_impl, url, options, cancel_token);
	});

	// Return the stream immediately (producer runs in background)
	asio::dispatch(asio::bind_executor(
		handler_ex, [handler = std::move(handler),
					 stream_impl = std::move(stream_impl)]() mutable {
			handler(outcome::success(AudioStream(std::move(stream_impl))));
		}));
}

}  // namespace ytdlpp::media
