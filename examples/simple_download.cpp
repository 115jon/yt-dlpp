#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <boost/asio.hpp>
#include <iostream>
#include <ytdlpp/downloader.hpp>
#include <ytdlpp/extractor.hpp>
#include <ytdlpp/http_client.hpp>

using namespace ytdlpp;

int main() {
	// Initialize logger
	auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
	auto logger = std::make_shared<spdlog::logger>("ytdlpp", console_sink);
	spdlog::set_default_logger(logger);
	spdlog::set_level(spdlog::level::debug);

	boost::asio::io_context ioc;
	auto work_guard = boost::asio::make_work_guard(ioc);

	// Components
	auto http = std::make_shared<net::HttpClient>(ioc.get_executor());
	auto extractor =
		std::make_unique<youtube::Extractor>(http, ioc.get_executor());
	auto downloader = std::make_unique<Downloader>(http);

	std::string url =
		"https://www.youtube.com/watch?v=F0tYP4OQ0-k";	// Example URL

	std::cout << "Extracting info for " << url << "...\n";

	// Async Extract
	extractor->async_process(url, [&](Result<VideoInfo> res) {
		if (res) {
			const auto &info = res.value();
			std::cout << "Extracted: " << info.title << "\n";
			std::cout << "Uploader: " << info.uploader << "\n";
			std::cout << "Duration: " << info.duration << "s\n";

			// Async Download
			std::cout << "Starting download (best video+audio)...\n";
			downloader->async_download(
				info, "best", "mp4",
				[](const std::string &status,
				   const ytdlpp::DownloadProgress &prog) {
					static auto last_print = std::chrono::steady_clock::now();
					auto now = std::chrono::steady_clock::now();
					if (std::chrono::duration_cast<std::chrono::milliseconds>(
							now - last_print)
								.count() > 500 ||
						prog.percentage >= 100.0) {
						std::cout
							<< "\r" << status << ": " << std::fixed
							<< std::setprecision(1) << prog.percentage << "% "
							<< "(" << prog.total_downloaded_bytes / 1024 / 1024
							<< "MB / " << prog.total_size_bytes / 1024 / 1024
							<< "MB) "
							<< "Speed: "
							<< prog.speed_bytes_per_sec / 1024 / 1024
							<< " MB/s "
							<< "ETA: " << (int)prog.eta_seconds << "s   "
							<< std::flush;
						last_print = now;
					}
				},
				[&](auto res) {
					if (res.has_error()) {
						spdlog::error(
							"Download failed: {}", res.error().message());
					} else {
						std::cout << "\nOperation complete.\n"
								  << "Downloaded to: " << res.value() << "\n";
					}
					work_guard.reset();
				});
		} else {
			std::cerr << "Extraction failed: " << res.error().message() << "\n";
			work_guard.reset();	 // Stop I/O context
		}
	});

	ioc.run();
	return 0;
}
