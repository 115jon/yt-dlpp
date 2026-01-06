#include <spdlog/spdlog.h>

#include <boost/asio.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <ytdlpp/audio_streamer.hpp>
#include <ytdlpp/downloader.hpp>
#include <ytdlpp/extractor.hpp>
#include <ytdlpp/http_client.hpp>

using namespace ytdlpp;

int main() {
	// Setup spdlog
	spdlog::set_level(spdlog::level::info);
	spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");

	// Create I/O context
	boost::asio::io_context ioc;
	auto work_guard = boost::asio::make_work_guard(ioc);

	// Run I/O context in a separate thread
	std::thread io_thread([&ioc]() { ioc.run(); });

	// Dependencies
	auto http = std::make_shared<net::HttpClient>(ioc.get_executor());
	auto extractor =
		std::make_unique<youtube::Extractor>(http, ioc.get_executor());

	// Hardcoded URL for example
	std::string url = "https://www.youtube.com/watch?v=F0tYP4OQ0-k";

	std::cout << "Extracting info for " << url << "...\n";

	extractor->async_process(url, [&](Result<VideoInfo> res) {
		if (res.has_error()) {
			std::cerr << "Extraction failed: " << res.error().message() << "\n";
			work_guard.reset();
			return;
		}

		const auto &info = res.value();
		std::cout << "Extracted: " << info.title << "\n";

		// Select best audio stream
		auto stream_info = Downloader::select_streams(info, "bestaudio");
		if (stream_info.audio) {
			std::cout << "Audio URL found: " << stream_info.audio->url << "\n";

			static std::ofstream pcm_file(
				"streamed_output.pcm", std::ios::binary);

			// Timer to stop streaming after 10 seconds
			static boost::asio::steady_timer stop_timer(ioc);
			stop_timer.expires_after(std::chrono::seconds(10));

			static boost::asio::cancellation_signal cancel_sig;

			static auto streamer =
				std::make_unique<media::AudioStreamer>(ioc.get_executor());

			media::AudioStreamer::AudioStreamOptions opts;
			opts.sample_rate = 48000;
			opts.channels = 2;
			opts.sample_fmt = media::AudioStreamer::SampleFormat::S16;

			std::cout << "Starting stream to 'streamed_output.pcm' (s16le, "
						 "48000Hz, 2ch)....\n";
			std::cout << "Streaming will run for 10 seconds...\n";

			streamer->async_stream(
				stream_info.audio->url, opts,
				[](const std::vector<uint8_t> &data) {
					if (pcm_file.is_open()) {
						pcm_file.write(
							reinterpret_cast<const char *>(data.data()),
							data.size());
					}
				},
				boost::asio::bind_cancellation_slot(
					cancel_sig.slot(), [&](Result<void> res) {
						if (res.has_error()) {
							if (res.error() ==
								boost::asio::error::make_error_code(
									boost::asio::error::operation_aborted)) {
								std::cout << "Streaming cancelled (timeout "
											 "reached).\n";
							} else {
								std::cerr << "Streaming failed: "
										  << res.error().message() << "\n";
							}
						} else {
							std::cout << "Streaming finished.\n";
						}
						stop_timer
							.cancel();	// Cancel timer if stream finishes early
						work_guard.reset();
					}));

			// Start timer
			stop_timer.async_wait([&](const boost::system::error_code &ec) {
				if (!ec) {
					std::cout << "Time limit reached, cancelling stream...\n";
					cancel_sig.emit(boost::asio::cancellation_type::all);
				}
			});

		} else {
			std::cerr << "No audio stream found.\n";
			work_guard.reset();
		}
	});

	if (io_thread.joinable()) io_thread.join();

	return 0;
}
