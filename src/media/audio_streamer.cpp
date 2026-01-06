#include <spdlog/spdlog.h>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/scope_exit.hpp>
#include <ytdlpp/audio_streamer.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace ytdlpp::media {

// Interrupt callback for FFmpeg - allows responsive cancellation during I/O
static int interrupt_callback(void *ctx) {
	auto *cancel_token = static_cast<std::atomic<bool> *>(ctx);
	return cancel_token->load() ? 1 : 0;  // Return 1 to interrupt
}

static bool stream_sync(const std::string &url,
						AudioStreamer::AudioStreamOptions options_in,
						const AudioStreamer::DataCallback &on_data,
						std::atomic<bool> &cancel_token) {
	AVFormatContext *format_ctx = nullptr;
	AVCodecContext *codec_ctx = nullptr;
	SwrContext *swr_ctx = nullptr;
	AVPacket *packet = nullptr;
	AVFrame *frame = nullptr;
	AVFrame *frame_resampled = nullptr;

	BOOST_SCOPE_EXIT_ALL(
		&frame_resampled, &frame, &packet, &swr_ctx, &codec_ctx, &format_ctx) {
		if (frame_resampled) av_frame_free(&frame_resampled);
		if (frame) av_frame_free(&frame);
		if (packet) av_packet_free(&packet);
		if (swr_ctx) swr_free(&swr_ctx);
		if (codec_ctx) avcodec_free_context(&codec_ctx);
		if (format_ctx) avformat_close_input(&format_ctx);
	};

	// Allocate format context and set interrupt callback BEFORE opening input
	format_ctx = avformat_alloc_context();
	if (!format_ctx) {
		spdlog::error("AudioStreamer: Failed to allocate format context");
		return false;
	}

	format_ctx->interrupt_callback.callback = interrupt_callback;
	format_ctx->interrupt_callback.opaque = &cancel_token;

	// Options for network
	{
		AVDictionary *options = nullptr;
		av_dict_set(&options, "reconnect", "1", 0);
		av_dict_set(&options, "reconnect_streamed", "1", 0);
		av_dict_set(&options, "reconnect_at_eof", "1", 0);
		av_dict_set(&options, "reconnect_delay_max", "5", 0);
		av_dict_set(
			&options, "user_agent",
			"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
			"(KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36",
			0);

		BOOST_SCOPE_EXIT_ALL(&options) { av_dict_free(&options); };

		if (avformat_open_input(&format_ctx, url.c_str(), nullptr, &options) <
			0) {
			spdlog::error("AudioStreamer: Failed to open input URL");
			return false;
		}
	}

	if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
		spdlog::error("AudioStreamer: Failed to find stream info");
		return false;
	}

	// Find best audio stream
	int stream_idx =
		av_find_best_stream(format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	if (stream_idx < 0) {
		spdlog::error("AudioStreamer: No audio stream found");
		return false;
	}

	AVStream *stream = format_ctx->streams[stream_idx];
	const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
	if (!decoder) {
		spdlog::error("AudioStreamer: Codec not found");
		return false;
	}

	codec_ctx = avcodec_alloc_context3(decoder);
	avcodec_parameters_to_context(codec_ctx, stream->codecpar);

	if (avcodec_open2(codec_ctx, decoder, nullptr) < 0) {
		spdlog::error("AudioStreamer: Failed to open codec");
		return false;
	}

	// Set up Resampler from options
	int dst_rate = options_in.sample_rate;
	AVSampleFormat dst_sample_fmt =
		static_cast<AVSampleFormat>(options_in.sample_fmt);

	swr_ctx = swr_alloc();

	// Input
	av_opt_set_chlayout(
		swr_ctx, "in_chlayout", &stream->codecpar->ch_layout, 0);
	av_opt_set_int(swr_ctx, "in_sample_rate", stream->codecpar->sample_rate, 0);
	av_opt_set_sample_fmt(
		swr_ctx, "in_sample_fmt", (AVSampleFormat)stream->codecpar->format, 0);

	// Output
	AVChannelLayout out_layout;
	av_channel_layout_default(&out_layout, options_in.channels);
	av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_layout, 0);
	av_opt_set_int(swr_ctx, "out_sample_rate", dst_rate, 0);
	av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", dst_sample_fmt, 0);

	if (swr_init(swr_ctx) < 0) {
		spdlog::error("AudioStreamer: Failed to init resampler");
		return false;
	}

	packet = av_packet_alloc();
	frame = av_frame_alloc();

	// Pre-allocate destination buffer for resampling
	// Estimate initial size based on typical frame size (1024 samples)
	const int initial_samples = 2048;
	uint8_t **dst_data = nullptr;
	int dst_linesize = 0;
	int allocated_samples = initial_samples;

	av_samples_alloc_array_and_samples(
		&dst_data, &dst_linesize, options_in.channels, allocated_samples,
		static_cast<AVSampleFormat>(options_in.sample_fmt), 0);

	BOOST_SCOPE_EXIT_ALL(&dst_data) {
		if (dst_data) {
			av_freep(&dst_data[0]);
			av_freep(&dst_data);
		}
	};

	// Pre-allocate output buffer for callback
	std::vector<uint8_t> buffer;
	buffer.reserve(
		allocated_samples * options_in.channels * 2);  // 2 bytes per S16 sample

	while (!cancel_token.load(std::memory_order_relaxed)) {
		int ret = av_read_frame(format_ctx, packet);
		if (ret < 0) break;	 // EOF or error

		if (packet->stream_index == stream_idx) {
			ret = avcodec_send_packet(codec_ctx, packet);
			if (ret >= 0) {
				while (ret >= 0) {
					ret = avcodec_receive_frame(codec_ctx, frame);
					if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) { break; }
					if (ret < 0) {
						spdlog::error("AudioStreamer: Error during decoding");
						break;
					}

					// Calculate required output samples
					int max_dst_nb_samples = static_cast<int>(av_rescale_rnd(
						swr_get_delay(swr_ctx, codec_ctx->sample_rate) +
							frame->nb_samples,
						dst_rate, codec_ctx->sample_rate, AV_ROUND_UP));

					// Reallocate if needed (rare after first few frames)
					if (max_dst_nb_samples > allocated_samples) {
						av_freep(&dst_data[0]);
						av_freep(&dst_data);
						allocated_samples = max_dst_nb_samples;
						av_samples_alloc_array_and_samples(
							&dst_data, &dst_linesize, options_in.channels,
							allocated_samples,
							static_cast<AVSampleFormat>(options_in.sample_fmt),
							0);
					}

					int nb_samples = swr_convert(
						swr_ctx, dst_data, max_dst_nb_samples,
						(const uint8_t **)frame->data, frame->nb_samples);

					if (nb_samples > 0) {
						int size = av_samples_get_buffer_size(
							nullptr, options_in.channels, nb_samples,
							static_cast<AVSampleFormat>(options_in.sample_fmt),
							1);

						// Reuse buffer, resize only if needed
						buffer.resize(static_cast<size_t>(size));
						std::memcpy(buffer.data(), dst_data[0],
									static_cast<size_t>(size));

						if (on_data) { on_data(buffer); }
					}
				}
			}
		}

		av_packet_unref(packet);
	}

	return true;
}

struct AudioStreamer::Impl {
	asio::any_io_executor ex;
	std::shared_ptr<std::atomic<bool>> cancel_token;
	asio::thread_pool work_pool{
		1};	 // Persistent worker thread for blocking ops

	explicit Impl(asio::any_io_executor e)
		: ex(std::move(e)),
		  cancel_token(std::make_shared<std::atomic<bool>>(false)) {}

	~Impl() {
		cancel();
		work_pool.join();  // Wait for pending work to complete
	}

	void cancel() const {
		if (cancel_token) *cancel_token = true;
	}

	void async_stream(std::string url, AudioStreamOptions options,
					  DataCallback on_data,
					  asio::any_completion_handler<void(Result<void>)> handler,
					  CompletionExecutor handler_ex,
					  asio::cancellation_slot slot) {
		// Cancel any previous stream
		cancel();

		// New token for new stream
		cancel_token = std::make_shared<std::atomic<bool>>(false);
		auto token = cancel_token;

		// Bind cancellation slot
		if (slot.is_connected()) {
			slot.assign([token](asio::cancellation_type type) {
				if (type != asio::cancellation_type::none) *token = true;
			});
		}

		// Post blocking work to the persistent thread pool
		asio::post(work_pool, [url = std::move(url), options,
							   on_data = std::move(on_data), token,
							   handler = std::move(handler),
							   handler_ex = std::move(handler_ex)]() mutable {
			// Check cancel before starting
			if (*token) {
				return asio::dispatch(
					handler_ex, [handler = std::move(handler)]() mutable {
						handler(
							outcome::failure(asio::error::operation_aborted));
					});
			}

			bool result = stream_sync(url, options, on_data, *token);

			// Dispatch completion - execute handler directly with result
			asio::dispatch(
				handler_ex,
				[result, token, handler = std::move(handler)]() mutable {
					// Check if cancelled during execution
					if (*token)
						handler(
							outcome::failure(asio::error::operation_aborted));
					else if (result)
						handler(outcome::success());
					else
						handler(outcome::failure(errc::request_failed));
				});
		});
	}
};

AudioStreamer::AudioStreamer(asio::any_io_executor ex)
	: m_impl(std::make_unique<Impl>(std::move(ex))) {}

void AudioStreamer::cancel() { m_impl->cancel(); }

AudioStreamer::~AudioStreamer() = default;
AudioStreamer::AudioStreamer(AudioStreamer &&) noexcept = default;
AudioStreamer &AudioStreamer::operator=(AudioStreamer &&) noexcept = default;

asio::any_io_executor AudioStreamer::get_executor() const { return m_impl->ex; }

void AudioStreamer::async_stream_impl(
	std::string url, AudioStreamOptions options, DataCallback on_data,
	asio::any_completion_handler<void(Result<void>)> handler,
	CompletionExecutor handler_ex, asio::cancellation_slot slot) {
	m_impl->async_stream(std::move(url), options, std::move(on_data),
						 std::move(handler), std::move(handler_ex), slot);
}

}  // namespace ytdlpp::media