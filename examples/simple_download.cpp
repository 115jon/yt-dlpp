#include <spdlog/spdlog.h>	// Include spdlog

#include <boost/asio.hpp>
#include <iostream>
#include <ytdlpp/ytdlpp.hpp>

using namespace ytdlpp;

int main() {
	boost::asio::io_context ioc;

	// Create library instance bound to executor
	spdlog::set_level(spdlog::level::debug);  // Enable verbose logging
	auto ytdl = Ytdlpp::create(ioc.get_executor());

	std::string url =
		"https://www.youtube.com/"
		"watch?v=pO_pD2GENGo&pp=ugUEEgJlbg%3D%3D";	// Example URL

	std::cout << "Extracting info for " << url << "...\n";

	auto work_guard = boost::asio::make_work_guard(ioc);

	// Async Extract
	ytdl.async_extract(url, [&](Result<VideoInfo> res) {
		if (res) {
			const auto &info = res.value();
			std::cout << "Extracted: " << info.title << "\n";
			std::cout << "Uploader: " << info.uploader << "\n";
			std::cout << "Duration: " << info.duration << "s\n";

			// Async Download
			std::cout << "Starting download (best video+audio)...\n";
			ytdl.async_download(
				info, "best", std::nullopt,
				[](const std::string &status, int progress) {
					std::cout << "\r" << status << ": " << progress << "%   "
							  << std::flush;
				},
				[&](Result<void> dl_res) {
					if (dl_res)
						std::cout << "Download complete!\n";
					else
						std::cerr
							<< "Download failed: " << dl_res.error().message()
							<< "\n";
					work_guard.reset();	 // Stop I/O context
				});
		} else {
			std::cerr << "Extraction failed: " << res.error().message() << "\n";
			work_guard.reset();	 // Stop I/O context
		}
	});

	ioc.run();
	return 0;
}
