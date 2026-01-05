#include <fmt/format.h>
#include <quickjs.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/program_options.hpp>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

// Windows headers for console
#ifdef _WIN32
#include <windows.h>
#endif

#include "downloader/downloader.hpp"
#include "media/muxer.hpp"
#include "net/http_client.hpp"
#include "scripting/quickjs_engine.hpp"
#include "youtube/extractor.hpp"

namespace po = boost::program_options;

// Helper for colored printing
#include <fmt/color.h>

void print_formats_table(std::vector<ytdlpp::youtube::VideoFormat> formats) {
	// Deduplicate formats by itag first
	std::sort(formats.begin(), formats.end(),
			  [](const auto &a, const auto &b) { return a.itag < b.itag; });
	auto last = std::unique(
		formats.begin(), formats.end(),
		[](const auto &a, const auto &b) { return a.itag == b.itag; });
	formats.erase(last, formats.end());

	// Sort formats: resolution (ascending), then tbr (ascending)
	std::sort(formats.begin(), formats.end(), [](const auto &a, const auto &b) {
		if (a.width * a.height != b.width * b.height)
			return a.width * a.height < b.width * b.height;
		return a.tbr < b.tbr;
	});

	// Header
	// yt-dlp Style:
	// Columns: Yellow
	// Bars: Blue
	// ID  EXT   RESOLUTION FPS CH │   FILESIZE   TBR PROTO │ VCODEC VBR ACODEC
	// ABR ASR MORE INFO
	auto blue_bar = fmt::format(fg(fmt::color::dodger_blue), "|");
	auto yellow_col = fg(fmt::color::yellow);

	fmt::print(yellow_col, "{:<4} {:<5} {:<11} {:>3} {:>2} ", "ID", "EXT",
			   "RESOLUTION", "FPS", "CH");
	fmt::print("{} ", blue_bar);
	fmt::print(yellow_col, "{:<9} {:>4} {:<5} ", "FILESIZE", "TBR", "PROTO");
	fmt::print("{} ", blue_bar);
	fmt::print(yellow_col, "{:<16} {:<12} {:>3} {:>3} {}\n", "VCODEC", "ACODEC",
			   "ABR", "ASR", "MORE INFO");

	// Separator line
	fmt::print(fg(fmt::color::white), "{:-^110}\n", "");

	for (const auto &f : formats) {
		// ID - Green
		std::string id = std::to_string(f.itag);

		// Ext
		std::string ext = f.ext;
		if (ext.empty()) ext = "unk";

		// Resolution
		std::string res = (f.vcodec != "none" && f.width > 0)
							  ? fmt::format("{}x{}", f.width, f.height)
							  : "audio only";

		// FPS
		std::string fps = (f.fps > 0) ? std::to_string(f.fps) : "";

		// CH
		std::string ch =
			(f.audio_channels > 0) ? std::to_string(f.audio_channels) : "";

		// Filesize
		std::string size = "~";
		if (f.content_length > 0) {
			double mib = (double)f.content_length / 1024.0 / 1024.0;
			size = fmt::format("{:.2f}MiB", mib);
		}

		// TBR
		std::string tbr = (f.tbr > 0) ? fmt::format("{}k", (int)f.tbr) : "N/A";

		// Proto
		std::string proto = "https";  // Assuming https usually
		if (f.url.find("m3u8") != std::string::npos) proto = "m3u8";

		// VCodec
		std::string vc = f.vcodec;
		if (vc.length() > 16) vc = vc.substr(0, 13) + "...";
		if (vc == "none") vc = "images";

		// ACodec
		std::string ac = f.acodec;
		if (ac.length() > 12) ac = ac.substr(0, 9) + "...";

		// Info - Grey if audio/video only
		std::string info;
		bool is_grey = false;
		if (f.vcodec != "none" && f.acodec == "none") {
			info += "video only";
			is_grey = true;
		} else if (f.vcodec == "none" && f.acodec != "none") {
			info += "audio only";
			is_grey = true;
		}

		if (f.height > 0) info += fmt::format(", {}p", f.height);

		// Print Row
		fmt::print(fg(fmt::color::green), "{:<4} ", id);
		fmt::print("{:<5} ", ext);
		fmt::print(is_grey ? fg(fmt::color::gray) : fg(fmt::color::white),
				   "{:<11} ", res);
		fmt::print("{:>3} {:>2} ", fps, ch);
		fmt::print("{} ", blue_bar);
		fmt::print("{:<9} {:>4} {:<5} ", size, tbr, proto);
		fmt::print("{} ", blue_bar);
		fmt::print("{:<16} {:<12} {:>3} {:>3} ", vc, ac,
				   (f.abr > 0 ? std::to_string((int)f.abr) + "k" : ""),
				   (f.audio_sample_rate > 0
						? std::to_string(f.audio_sample_rate / 1000) + "k"
						: ""));
		fmt::print(is_grey ? fg(fmt::color::gray) : fg(fmt::color::white),
				   "{}\n", info);
	}
}

int main(int argc, char *argv[]) {
#ifdef _WIN32
	SetConsoleOutputCP(CP_UTF8);
#endif

	try {
		// Setup spdlog to look like yt-dlp: [youtube] message
		spdlog::set_pattern("[youtube] %v");

		po::options_description desc("Allowed options");
		desc.add_options()("help,h", "produce help message")(
			"url", po::value<std::string>(), "URL to download")(
			"format,f", po::value<std::string>(), "Format selector")(
			"list-formats,F", "List available formats")(
			"get-url,g", "Print URL")(
			"merge-output-format", po::value<std::string>(),
			"Output format for merging (e.g. mkv, "
			"mp4, webm)")(
			"manual-merge", po::value<std::vector<std::string>>()->multitoken(),
			"Manually merge video and audio: --manual-merge <video> <audio> "
			"<output>")("verbose,v", "Enable verbose logging");

		po::positional_options_description p;
		p.add("url", 1);

		po::variables_map vm;
		po::store(po::command_line_parser(argc, argv)
					  .options(desc)
					  .positional(p)
					  .run(),
				  vm);
		po::notify(vm);

		if (vm.count("help")) {
			std::cout << "Usage: yt-dlpp [options] <url>\n" << desc << "\n";
			return 0;
		}

		if (vm.count("verbose")) {
			spdlog::set_level(spdlog::level::debug);
		} else {
			spdlog::set_level(spdlog::level::info);
		}

		// Handle Manual Merge command first
		if (vm.count("manual-merge")) {
			auto args = vm["manual-merge"].as<std::vector<std::string>>();
			if (args.size() != 3) {
				spdlog::error(
					"Usage: --manual-merge <video_path> <audio_path> "
					"<output_path>");
				return 1;
			}
			spdlog::info(
				"Manually merging...\nVideo: {}\nAudio: {}\nOutput: {}",
				args[0], args[1], args[2]);
			if (ytdlpp::media::Muxer::merge(args[0], args[1], args[2])) {
				spdlog::info("Merge successful!");
				return 0;
			}
			spdlog::error("Merge failed.");
			return 1;
		}

		if (vm.count("url")) {
			std::string url = vm["url"].as<std::string>();

			boost::asio::io_context ioc;
			ytdlpp::net::HttpClient client(ioc);
			ytdlpp::scripting::JsEngine js;
			ytdlpp::youtube::Extractor extractor(client, js);

			auto result_info = extractor.process(url);

			if (result_info.has_value()) {
				const auto &info = result_info.value();
				if (vm.count("list-formats")) {
					print_formats_table(info.formats);
					return 0;
				}

				if (vm.count("get-url")) {
					// Selection logic (get-url mode)
					std::string format =
						vm.count("format") ? vm["format"].as<std::string>()
										   : "best";

					ytdlpp::Downloader downloader(client);
					auto streams = downloader.select_streams(info, format);

					if (streams.video) {
						std::cout << streams.video->url << "\n";
					}
					if (streams.audio && streams.audio != streams.video) {
						std::cout << streams.audio->url << "\n";
					}

					if (!streams.video && !streams.audio) {
						std::cerr << "Format not found\n";
						return 1;
					}
					return 0;
				}

				// Download mode
				ytdlpp::Downloader downloader(client);
				std::string selector =
					vm.count("format") ? vm["format"].as<std::string>()
									   : "best";
				std::optional<std::string> merge_fmt = std::nullopt;
				if (vm.count("merge-output-format")) {
					merge_fmt = vm["merge-output-format"].as<std::string>();
				}

				if (downloader.download(info, selector, merge_fmt)) {
					spdlog::info("Operation complete.");
					return 0;
				}
				spdlog::error("Download failed.");
				return 1;
			}
			spdlog::error("Failed to extract video info: {}",
						  result_info.error().message());
			return 1;
		}
		std::cout << "Usage: yt-dlpp [options] <url>\n" << desc << "\n";
		return 1;

	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << "\n";
		return 1;
	}
}
