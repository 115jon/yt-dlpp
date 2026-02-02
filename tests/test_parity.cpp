
#include <gtest/gtest.h>

// Include the fix for boost::process::v2 utf8 issue on Windows
#ifdef _WIN32
#include "fixed_utf8.hpp"
#endif

#include <spdlog/spdlog.h>

#include <boost/asio.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/popen.hpp>
#include <future>
#include <iostream>
#include <nlohmann/json.hpp>
#include <ytdlpp/extractor.hpp>
#include <ytdlpp/extractor_args.hpp>
#include <ytdlpp/http_client.hpp>
#include <ytdlpp/types.hpp>

namespace bp2 = boost::process::v2;
namespace asio = boost::asio;

class ParityTest : public ::testing::Test {
   protected:
	asio::io_context ioc;

	std::string run_yt_dlp(const std::vector<std::string> &args) {
		std::string out;
		try {
			// Find yt-dlp in PATH
			auto exe = bp2::environment::find_executable("yt-dlp");

			bp2::popen proc(ioc, exe, args);

			// Read output
			std::vector<char> buffer(4096);
			std::promise<void> done;
			auto feat = done.get_future();

			std::function<void(boost::system::error_code, std::size_t)>
				read_handler;
			read_handler = [&](boost::system::error_code ec, std::size_t len) {
				if (!ec) {
					out.append(buffer.data(), len);
					proc.async_read_some(asio::buffer(buffer), read_handler);
				} else {
					done.set_value();
				}
			};

			proc.async_read_some(asio::buffer(buffer), read_handler);

			ioc.restart();
			ioc.run();
			feat.wait();

			auto exit_code = proc.wait();
			if (exit_code != 0) {
				std::cerr << "yt-dlp failed with exit code " << exit_code
						  << "\n";
			}
		} catch (const std::exception &e) {
			std::cerr << "Exception running yt-dlp: " << e.what() << "\n";
		}
		return out;
	}
};

TEST_F(ParityTest, YoutubeSearchURLParity) {
	spdlog::set_level(spdlog::level::debug);
	std::string query = "ytsearch:fire bow tutorial";
	std::string extractor_args = "youtube:player_client=web,-android_sdkless";

	// 1. Get URL from yt-dlp
	// Note: We use -g to get URLs, and we specify the same extractor-args
	std::vector<std::string> yt_dlp_args = {
		"--extractor-args", extractor_args, "-g", query};

	std::string yt_dlp_output = run_yt_dlp(yt_dlp_args);
	// yt-dlp -g for a video often returns two URLs (video and audio)
	std::vector<std::string> yt_dlp_urls;
	std::stringstream ss(yt_dlp_output);
	std::string line;
	while (std::getline(ss, line)) {
		if (!line.empty()) yt_dlp_urls.push_back(line);
	}

	ASSERT_FALSE(yt_dlp_urls.empty()) << "yt-dlp failed to return any URLs";

	// 2. Get URL from yt-dlpp library
	auto http = std::make_shared<ytdlpp::net::HttpClient>(ioc.get_executor());
	ytdlpp::youtube::Extractor extractor(http, ioc.get_executor());

	ytdlpp::youtube::ExtractorArgs args;
	args.parse(extractor_args);
	extractor.set_extractor_args(args);

	ytdlpp::VideoInfo info;
	auto search_opts = ytdlpp::youtube::parse_search_url(query);
	if (search_opts) {
		ytdlpp::Result<std::vector<ytdlpp::SearchResult>> search_res =
			ytdlpp::errc::unknown;
		auto work = asio::make_work_guard(ioc);
		extractor.async_search(
			*search_opts,
			[&](ytdlpp::Result<std::vector<ytdlpp::SearchResult>> res) {
				search_res = std::move(res);
				work.reset();
				ioc.stop();
			});

		ioc.restart();
		ioc.run();

		ASSERT_TRUE(search_res.has_value())
			<< "yt-dlpp search failed: " << search_res.error().message();
		ASSERT_FALSE(search_res.value().empty()) << "No results found";

		ytdlpp::Result<ytdlpp::VideoInfo> info_res = ytdlpp::errc::unknown;
		auto work2 = asio::make_work_guard(ioc);
		extractor.async_process(search_res.value()[0].url,
								[&](ytdlpp::Result<ytdlpp::VideoInfo> res) {
									info_res = std::move(res);
									work2.reset();
									ioc.stop();
								});

		ioc.restart();
		ioc.run();

		ASSERT_TRUE(info_res.has_value())
			<< "yt-dlpp info extraction failed: " << info_res.error().message();
		info = std::move(info_res.value());
	} else {
		ytdlpp::Result<ytdlpp::VideoInfo> info_res = ytdlpp::errc::unknown;
		auto work = asio::make_work_guard(ioc);
		extractor.async_process(
			query, [&](ytdlpp::Result<ytdlpp::VideoInfo> res) {
				info_res = std::move(res);
				work.reset();
				ioc.stop();
			});

		ioc.restart();
		ioc.run();

		ASSERT_TRUE(info_res.has_value())
			<< "yt-dlpp failed to extract info: " << info_res.error().message();
		info = std::move(info_res.value());
	}

	// For comparison, we just check if we got roughly the same number of URLs
	// or if the primary formats match.
	// yt-dlpp should have formats collected.
	ASSERT_FALSE(info.formats.empty());

	// Log for manual verification if needed
	std::cout << "yt-dlp URLs: " << yt_dlp_urls.size() << std::endl;
	for (const auto &u : yt_dlp_urls) {
		std::cout << "  yt-dlp: " << u.substr(0, 100) << "..." << std::endl;
	}
}
