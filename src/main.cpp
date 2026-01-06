#include <fmt/format.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/program_options.hpp>
#include <csignal>
#include <iostream>
#include <string>

// Windows headers for console
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#include <fmt/color.h>

#include <ytdlpp/audio_streamer.hpp>
#include <ytdlpp/downloader.hpp>
#include <ytdlpp/extractor.hpp>
#include <ytdlpp/http_client.hpp>
#include <ytdlpp/types.hpp>

#include "media/muxer.hpp"

// Signal handling
std::shared_ptr<std::atomic<bool>> g_cancel_token =
	std::make_shared<std::atomic<bool>>(false);

void signal_handler(int) {
	if (g_cancel_token) g_cancel_token->store(true);
}

namespace po = boost::program_options;

void print_formats_table(std::vector<ytdlpp::VideoFormat> formats) {
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

	// Build table data first to calculate column widths
	struct Row {
		std::string id, ext, res, fps, ch, size, tbr, proto, vcodec, acodec,
			abr, asr, info;
		bool is_grey = false;
	};
	std::vector<Row> rows;

	for (const auto &f : formats) {
		Row r;
		r.id = std::to_string(f.itag);
		r.ext = f.ext.empty() ? "unk" : f.ext;
		r.res = (f.vcodec != "none" && f.width > 0)
					? fmt::format("{}x{}", f.width, f.height)
					: "audio only";
		r.fps = (f.fps > 0) ? std::to_string(f.fps) : "";
		r.ch = (f.audio_channels > 0) ? std::to_string(f.audio_channels) : "";

		if (f.content_length > 0) {
			double mib = (double)f.content_length / 1024.0 / 1024.0;
			r.size = fmt::format("{:.2f}MiB", mib);
		} else {
			r.size = "~";
		}

		r.tbr = (f.tbr > 0) ? fmt::format("{}k", (int)f.tbr) : "";
		r.proto = "https";
		if (f.url.find("m3u8") != std::string::npos) r.proto = "m3u8";

		r.vcodec = f.vcodec;
		if (r.vcodec.length() > 16) r.vcodec = r.vcodec.substr(0, 13) + "...";
		if (r.vcodec == "none") r.vcodec = "images";

		r.acodec = f.acodec;
		if (r.acodec.length() > 12) r.acodec = r.acodec.substr(0, 9) + "...";

		r.abr = (f.abr > 0) ? std::to_string((int)f.abr) + "k" : "";
		r.asr = (f.audio_sample_rate > 0)
					? std::to_string(f.audio_sample_rate / 1000) + "k"
					: "";

		if (f.vcodec != "none" && f.acodec == "none") {
			r.info = "video only";
			r.is_grey = true;
		} else if (f.vcodec == "none" && f.acodec != "none") {
			r.info = "audio only";
			r.is_grey = true;
		}
		if (f.height > 0) r.info += fmt::format(", {}p", f.height);

		rows.push_back(r);
	}

	// Header - matching yt-dlp format
	fmt::print(
		"ID  EXT   RESOLUTION FPS CH |   FILESIZE    TBR PROTO | VCODEC        "
		"   VBR ACODEC      ABR ASR MORE INFO\n");
	fmt::print("{:-^119}\n", "");

	// Print rows with proper alignment matching yt-dlp
	for (const auto &r : rows) {
		auto info_color =
			r.is_grey ? fg(fmt::color::gray) : fg(fmt::color::white);

		// ID left-aligned 3 chars, EXT 5 chars, RES 10 chars, FPS 3 right, CH 2
		// right
		fmt::print("{:<3} {:<5} ", r.id, r.ext);
		fmt::print(info_color, "{:<10} ", r.res);
		fmt::print("{:>3} {:>2} | ", r.fps, r.ch);

		// FILESIZE 10 right, TBR 6 right, PROTO 5
		fmt::print("{:>10} {:>6} {:<5} | ", r.size, r.tbr, r.proto);

		// VCODEC 16, VBR 3, ACODEC 11, ABR 3, ASR 3
		fmt::print("{:<16} {:>3} {:<11} {:>3} {:>3} ", r.vcodec, "", r.acodec,
				   r.abr, r.asr);

		// MORE INFO
		fmt::print(info_color, "{}\n", r.info);
	}
}

int main(int argc, char *argv[]) {
#ifdef _WIN32
	SetConsoleOutputCP(CP_UTF8);
#endif

	try {
		// Create a stderr logger and set as default
		auto stderr_logger = spdlog::stderr_color_mt("stderr");
		spdlog::set_default_logger(stderr_logger);
		// Setup spdlog to look like yt-dlp: [youtube] message
		spdlog::set_pattern("[youtube] %v");

		po::options_description desc("Allowed options");
		desc.add_options()("help,h", "produce help message")(
			"url", po::value<std::string>(), "URL to download")(
			"format,f", po::value<std::string>(), "Format selector")(
			"list-formats,F", "List available formats")(
			"dump-json,j", "Output video info as JSON")(
			"get-url,g", "Print URL")(
			"merge-output-format", po::value<std::string>(),
			"Output format for merging (e.g. mkv, "
			"mp4, webm)")(
			"manual-merge", po::value<std::vector<std::string>>()->multitoken(),
			"Manually merge video and audio: --manual-merge <video> <audio> "
			"<output>")(
			"stream-audio", "Stream audio to stdout (s16le, 48kHz, stereo)")(
			"verbose,v", "Enable verbose logging");

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

			// Shared HTTP Client
			auto http =
				std::make_shared<ytdlpp::net::HttpClient>(ioc.get_executor());

			// Extractor
			ytdlpp::youtube::Extractor extractor(http, ioc.get_executor());

			std::optional<ytdlpp::Result<ytdlpp::VideoInfo>> result_info_opt;

			extractor.async_process(
				url, [&](ytdlpp::Result<ytdlpp::VideoInfo> res) {
					result_info_opt = std::move(res);
				});

			ioc.run();
			ioc.restart();

			if (!result_info_opt.has_value()) {
				fmt::println(stderr, "Extraction timed out or failed to run");
				return 1;
			}
			auto result_info = std::move(*result_info_opt);

			if (!result_info.has_value()) {
				fmt::println(stderr, "Failed to extract info: {}",
							 result_info.error().message());
				return 1;
			}
			const auto &info = result_info.value();

			if (vm.count("stream-audio")) {
				// Audio Streaming Mode
#ifdef _WIN32
				_setmode(_fileno(stdout), _O_BINARY);
#endif
				// Direct all logs to stderr so stdout is pure audio

				// Find best audio
				std::string audio_url;
				const ytdlpp::VideoFormat *best = nullptr;
				for (const auto &fmt : info.formats) {
					if (fmt.vcodec == "none" && fmt.acodec != "none") {
						if (!best || fmt.tbr > best->tbr) best = &fmt;
					}
				}
				if (best) audio_url = best->url;

				if (audio_url.empty()) {
					fmt::println(stderr, "No audio stream found.");
					return 1;
				}

				// Register signal handler for this scope
				std::signal(SIGINT, signal_handler);

				ytdlpp::media::AudioStreamer streamer(ioc.get_executor());

				ytdlpp::media::AudioStreamer::AudioStreamOptions opts;
				opts.sample_rate = 48000;
				opts.channels = 2;
				opts.sample_fmt =
					ytdlpp::media::AudioStreamer::SampleFormat::S16;

				streamer.async_stream(
					audio_url, opts,
					[](const std::vector<uint8_t> &data) {
						if (!data.empty()) {
							std::cout.write(
								reinterpret_cast<const char *>(data.data()),
								data.size());
							std::cout.flush();
						}
					},
					[&](ytdlpp::Result<void>) {});

				// Wait for stream to finish or cancel
				ioc.run();
				std::signal(SIGINT, SIG_DFL);  // Restore default

				return 0;
			}

			if (vm.count("dump-json")) {
				// Output video info as JSON (yt-dlp -j compatibility)
				nlohmann::json j;
				ytdlpp::youtube::to_json(j, info);
				std::cout << j.dump(2) << std::endl;
				return 0;
			}

			if (vm.count("list-formats")) {
				print_formats_table(info.formats);
				return 0;
			}

			if (vm.count("get-url")) {
				// Selection logic (get-url mode)
				std::string format =
					vm.count("format") ? vm["format"].as<std::string>()
									   : "best";

				ytdlpp::Downloader downloader(http);
				auto streams = downloader.select_streams(info, format);

				if (streams.video) { std::cout << streams.video->url << "\n"; }
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
			std::string selector =
				vm.count("format") ? vm["format"].as<std::string>() : "best";
			std::optional<std::string> merge_fmt = std::nullopt;
			if (vm.count("merge-output-format")) {
				merge_fmt = vm["merge-output-format"].as<std::string>();
			}

			ytdlpp::Downloader downloader(http);

			downloader.async_download(
				info, selector, merge_fmt,
				[](const std::string &status,
				   const ytdlpp::DownloadProgress &prog) {
					std::cout << "\r" << status << ": " << prog.percentage
							  << "%   " << std::flush;
				},
				[](ytdlpp::Result<std::string> res) {
					if (res)
						std::cout << "\nOperation complete.\n"
								  << res.value() << "\n";
					else
						std::cerr
							<< "\nDownload failed: " << res.error().message()
							<< "\n";
				});
			ioc.run();
			return 0;
		}

		std::cout << "Usage: yt-dlpp [options] <url>\n" << desc << "\n";
		return 1;
	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << "\n";
		return 1;
	}
}
