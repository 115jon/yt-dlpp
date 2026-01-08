#include <spdlog/spdlog.h>

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
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
		if (!stream_info.audio) {
			std::cerr << "No audio stream found.\n";
			work_guard.reset();
			return;
		}

		std::cout << "Audio URL found.\n";

		// Use spawn for coroutine-style streaming
		boost::asio::spawn(
			ioc.get_executor(),
			[&, audio_url =
					stream_info.audio->url](boost::asio::yield_context yield) {
				static std::ofstream pcm_file(
					"streamed_output.pcm", std::ios::binary);

				media::AudioStreamer streamer(ioc.get_executor());
				media::AudioStreamOptions opts;
				opts.sample_rate = 48000;
				opts.channels = 2;
				opts.sample_fmt = media::SampleFormat::S16;

				boost::system::error_code ec;

				// Open stream
				auto stream_result =
					streamer.async_open(audio_url, opts, yield[ec]);
				if (ec || stream_result.has_error()) {
					std::cerr
						<< "Failed to open stream: "
						<< (ec ? ec.message() : stream_result.error().message())
						<< "\n";
					work_guard.reset();
					return;
				}

				auto &stream = stream_result.value();

				std::cout << "Stream opened. Writing to 'streamed_output.pcm' "
							 "(s16le, 48000Hz, 2ch)...\n";

				// Read loop - like reading from a pipe!
				size_t total_bytes = 0;
				auto start = std::chrono::steady_clock::now();
				const auto max_duration = std::chrono::seconds(10);

				while (!stream.is_eof()) {
					// Check time limit
					auto elapsed = std::chrono::steady_clock::now() - start;
					if (elapsed > max_duration) {
						std::cout << "Time limit reached.\n";
						stream.cancel();
						break;
					}

					// Async read - yields until data available
					auto read_result = stream.async_read(yield[ec]);
					if (ec) {
						if (ec == boost::asio::error::operation_aborted) {
							std::cout << "Stream cancelled.\n";
						} else {
							std::cerr << "Read error: " << ec.message() << "\n";
						}
						break;
					}

					auto &data = read_result.value();
					if (data.empty()) {
						// EOF
						break;
					}

					// Write to file
					if (pcm_file.is_open()) {
						pcm_file.write(
							reinterpret_cast<const char *>(data.data()),
							static_cast<std::streamsize>(data.size()));
					}

					total_bytes += data.size();
				}

				std::cout << "Streaming finished. Total bytes: " << total_bytes
						  << "\n";
				work_guard.reset();
			},
			boost::asio::detached);
	});

	if (io_thread.joinable()) io_thread.join();

	return 0;
}
