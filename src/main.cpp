#include <fmt/format.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/coroutine/attributes.hpp>
#include <boost/program_options.hpp>
#include <iostream>
#include <string>
#include <utility>

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
#include <ytdlpp/output_template.hpp>
#include <ytdlpp/types.hpp>

#include "media/muxer.hpp"

namespace po = boost::program_options;
namespace asio = boost::asio;

// Global cancellation flag for async operations
static std::atomic<bool> g_cancelled{false};

// =============================================================================
// Format Table Printing
// =============================================================================

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

	constexpr double KIB = 1024.0;
	constexpr double MIB = KIB * 1024.0;
	constexpr double KBPS_SCALE = 1000.0;

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
			double mib = static_cast<double>(f.content_length) / MIB;
			r.size = fmt::format("{:.2f}MiB", mib);
		} else {
			r.size = "~";
		}

		r.tbr = (f.tbr > 0) ? fmt::format("{:.0f}k", f.tbr / KBPS_SCALE) : "";
		r.proto = f.protocol.empty() ? "https" : f.protocol;
		r.vcodec = f.vcodec.empty()		? "none"
				   : f.vcodec == "none" ? "video only"
										: f.vcodec.substr(0, 12);
		r.acodec = f.acodec.empty()		? "none"
				   : f.acodec == "none" ? "audio only"
										: f.acodec.substr(0, 8);
		r.abr = (f.acodec != "none" && f.tbr > 0)
					? fmt::format("{:.0f}k", f.tbr / KBPS_SCALE)
					: "";
		r.asr = (f.audio_sample_rate > 0)
					? fmt::format("{}Hz", f.audio_sample_rate)
					: "";
		r.info = f.format_note.empty() ? "" : f.format_note;
		r.is_grey = (f.vcodec == "none" || f.acodec == "none");
		rows.push_back(r);
	}

	// Print header and rows
	fmt::println(
		"{:<6} {:<5} {:<12} {:<4} {:>2} {:>10} {:>7} {:<5} {:<14} "
		"{:<10} {:>6} {:>7} {}",
		"ID", "EXT", "RESOLUTION", "FPS", "CH", "FILESIZE", "TBR", "PROTO",
		"VCODEC", "ACODEC", "|ABR", "ASR", "INFO");

	for (const auto &r : rows) {
		if (r.is_grey) {
			fmt::print(fg(fmt::color::dim_gray),
					   "{:<6} {:<5} {:<12} {:<4} {:>2} {:>10} {:>7} {:<5} "
					   "{:<14} {:<10} {:>6} {:>7} {}\n",
					   r.id, r.ext, r.res, r.fps, r.ch, r.size, r.tbr, r.proto,
					   r.vcodec, r.acodec, r.abr, r.asr, r.info);
		} else {
			fmt::println(
				"{:<6} {:<5} {:<12} {:<4} {:>2} {:>10} {:>7} {:<5} "
				"{:<14} {:<10} {:>6} {:>7} {}",
				r.id, r.ext, r.res, r.fps, r.ch, r.size, r.tbr, r.proto,
				r.vcodec, r.acodec, r.abr, r.asr, r.info);
		}
	}
}

// =============================================================================
// yt-dlp Compatible Output Formatting
// =============================================================================

// Log with proper prefix like yt-dlp
// Patterns: [youtube:search], [youtube], [download], [info]
void log_search(std::string_view msg) {
	fmt::println(stderr, "[youtube:search] {}", msg);
}

void log_download(std::string_view msg) {
	fmt::println(stderr, "[download] {}", msg);
}

void log_youtube(std::string_view msg) {
	fmt::println(stderr, "[youtube] {}", msg);
}

void log_info(std::string_view msg) { fmt::println(stderr, "[info] {}", msg); }

// =============================================================================
// CLI Application using Coroutines
// =============================================================================

struct CliOptions {
	std::string url;
	std::string format = "best";
	std::optional<std::string> merge_format;

	// New: output options
	std::string output_template = "%(title)s [%(id)s].%(ext)s";
	std::string output_path = ".";

	// New: audio extraction
	bool extract_audio = false;
	std::string audio_format;  // mp3, m4a, opus, etc.

	// New: display options
	bool quiet = false;
	bool simulate = false;
	std::string print_template;	 // -O print template

	// Existing options
	bool list_formats = false;
	bool dump_json = false;
	bool get_url = false;
	bool stream_audio = false;
	bool verbose = false;
	bool flat_playlist = false;
};

// Main application logic using yield_context for clean async
void run_app(asio::io_context &ioc,
			 const std::shared_ptr<ytdlpp::net::HttpClient> &http,
			 const CliOptions &opts, asio::yield_context yield) {
	ytdlpp::youtube::Extractor extractor(http, ioc.get_executor());

	// Check if this is a search URL
	auto search_opts = ytdlpp::youtube::parse_search_url(opts.url);
	if (search_opts) {
		log_search(fmt::format("Extracting URL: {}", opts.url));
		log_download(
			fmt::format("Downloading playlist: {}", search_opts->query));
		log_search(fmt::format(
			"query \"{}\": Downloading API JSON", search_opts->query));

		auto search_result = extractor.async_search(*search_opts, yield);

		if (search_result.has_error()) {
			fmt::println(stderr, "ERROR: Search failed: {}",
						 search_result.error().message());
			return;
		}

		const auto &results = search_result.value();
		log_search(fmt::format(
			"Playlist {}: Downloading {} items of {}", search_opts->query,
			results.size(), results.size()));

		if (results.empty()) {
			fmt::println(stderr, "ERROR: No results found for: \"{}\"",
						 search_opts->query);
			return;
		}

		// If --dump-json, output results as JSON
		if (opts.dump_json) {
			nlohmann::json j = nlohmann::json::array();
			for (const auto &r : results) {
				nlohmann::json item;
				ytdlpp::youtube::to_json(item, r);
				j.push_back(item);
			}
			std::cout << j.dump(2) << "\n";
			return;
		}

		// For --flat-playlist, just list items
		if (opts.flat_playlist) {
			for (size_t i = 0; i < results.size(); ++i) {
				log_download(fmt::format(
					"Downloading item {} of {}", i + 1, results.size()));
			}
			log_download(fmt::format(
				"Finished downloading playlist: {}", search_opts->query));
			return;
		}

		// Extract and process each video
		for (size_t i = 0; i < results.size(); ++i) {
			log_download(fmt::format(
				"Downloading item {} of {}", i + 1, results.size()));

			auto video_url = results[i].url;
			log_youtube(fmt::format("Extracting URL: {}", video_url));

			auto info_result = extractor.async_process(video_url, yield);
			if (info_result.has_error()) {
				fmt::println(stderr, "ERROR: Failed to extract {}: {}",
							 video_url, info_result.error().message());
				continue;
			}

			const auto &info = info_result.value();

			// If --get-url, print URLs
			if (opts.get_url) {
				auto streams =
					ytdlpp::Downloader::select_streams(info, opts.format);
				if (streams.video) { std::cout << streams.video->url << "\n"; }
				if (streams.audio && streams.audio != streams.video) {
					std::cout << streams.audio->url << "\n";
				}
				continue;
			}

			// Otherwise download
			ytdlpp::Downloader downloader(http);
			auto download_result = downloader.async_download(
				info, opts.format, opts.merge_format,
				[](const std::string &status,
				   const ytdlpp::DownloadProgress &prog) {
					std::cout << "\r" << status << ": " << prog.percentage
							  << "%   " << std::flush;
				},
				yield);

			if (download_result.has_error()) {
				fmt::println(stderr, "\nERROR: Download failed: {}",
							 download_result.error().message());
			} else {
				fmt::println(
					"\n[download] 100%% of {}", download_result.value());
			}
		}

		log_download(fmt::format(
			"Finished downloading playlist: {}", search_opts->query));
		return;
	}

	// Regular video extraction
	log_youtube(fmt::format("Extracting URL: {}", opts.url));

	auto info_result = extractor.async_process(opts.url, yield);
	if (info_result.has_error()) {
		fmt::println(stderr, "ERROR: Failed to extract info: {}",
					 info_result.error().message());
		return;
	}

	const auto &info = info_result.value();

	// Handle stream-audio mode
	if (opts.stream_audio) {
#ifdef _WIN32
		(void)_setmode(_fileno(stdout), _O_BINARY);
#endif
		// Find best audio
		const ytdlpp::VideoFormat *best = nullptr;
		for (const auto &f : info.formats) {
			if (f.vcodec == "none" && f.acodec != "none") {
				if (!best || f.tbr > best->tbr) best = &f;
			}
		}

		if (!best) {
			spdlog::error("No audio format found");
			return;
		}

		ytdlpp::media::AudioStreamOptions audio_opts;
		audio_opts.sample_rate = 48000;
		audio_opts.channels = 2;
		audio_opts.sample_fmt = ytdlpp::media::SampleFormat::S16;

		ytdlpp::media::AudioStreamer streamer(ioc.get_executor());
		auto stream = streamer.async_open(best->url, audio_opts, yield);
		if (stream.has_error()) {
			spdlog::error(
				"Failed to open stream: {}", stream.error().message());
			return;
		}

		auto &audio_stream = stream.value();
		while (!audio_stream.is_eof() && !g_cancelled.load()) {
			auto read_result = audio_stream.async_read(yield);
			if (read_result.has_error() || read_result.value().empty()) break;
			const auto &audio_data = read_result.value();
			std::fwrite(audio_data.data(), 1, audio_data.size(), stdout);
		}
		return;
	}

	// Handle --dump-json
	if (opts.dump_json) {
		nlohmann::json j;
		ytdlpp::youtube::to_json(j, info);
		std::cout << j.dump(2) << "\n";
		return;
	}

	// Handle --list-formats
	if (opts.list_formats) {
		print_formats_table(info.formats);
		return;
	}

	// Handle -O, --print (template printing)
	if (!opts.print_template.empty()) {
		auto output = ytdlpp::expand_output_template(opts.print_template, info);
		std::cout << output << "\n";
		return;
	}

	// Handle --get-url
	if (opts.get_url) {
		auto streams = ytdlpp::Downloader::select_streams(info, opts.format);

		if (streams.video) { std::cout << streams.video->url << "\n"; }
		if (streams.audio && streams.audio != streams.video) {
			std::cout << streams.audio->url << "\n";
		}

		if (!streams.video && !streams.audio) {
			fmt::println(stderr, "Format not found");
		}
		return;
	}

	// Select format and show info (like yt-dlp)
	auto streams = ytdlpp::Downloader::select_streams(info, opts.format);
	if (!streams.video && !streams.audio) {
		fmt::println(
			stderr, "ERROR: No matching format found for: {}", opts.format);
		return;
	}

	// Build format string for info line
	std::string format_str;
	if (streams.video && streams.audio && streams.video != streams.audio) {
		format_str = std::to_string(streams.video->itag) + "+" +
					 std::to_string(streams.audio->itag);
	} else if (streams.video) {
		format_str = std::to_string(streams.video->itag);
	} else if (streams.audio) {
		format_str = std::to_string(streams.audio->itag);
	}

	// Print info line like yt-dlp
	if (!opts.quiet) {
		log_info(fmt::format(
			"{}: Downloading 1 format(s): {}", info.id, format_str));
	}

	// Handle --simulate (don't download)
	if (opts.simulate) { return; }

	// Download mode
	ytdlpp::Downloader downloader(http);
	auto download_result = downloader.async_download(
		info, opts.format, opts.merge_format,
		[&opts](
			const std::string &status, const ytdlpp::DownloadProgress &prog) {
			if (!opts.quiet) {
				std::cout << "\r" << status << ": " << prog.percentage << "%   "
						  << std::flush;
			}
		},
		yield);

	if (download_result.has_error()) {
		fmt::println(stderr, "\nERROR: Download failed: {}",
					 download_result.error().message());
	} else {
		if (!opts.quiet) {
			fmt::println("\n[download] 100%% of {}", download_result.value());
		}
	}
}

// =============================================================================
// Main Entry Point
// =============================================================================

int main(int argc, char *argv[]) {
#ifdef _WIN32
	SetConsoleOutputCP(CP_UTF8);
#endif

	try {
		// Setup logging
		auto stderr_logger = spdlog::stderr_color_mt("stderr");
		spdlog::set_default_logger(stderr_logger);
		spdlog::set_pattern("[youtube] %v");

		// Parse command line
		po::options_description desc("Options");
		// clang-format off
		desc.add_options()
			("help,h", "Print help message")
			("url", po::value<std::string>(), "URL to download")
			// Format selection
			("format,f", po::value<std::string>()->default_value("best"),
			 "Format selector (e.g., best, bestaudio, 22+140)")
			("list-formats,F", "List available formats")
			// Audio extraction
			("extract-audio,x", "Convert video to audio-only file")
			("audio-format", po::value<std::string>(),
			 "Audio format to convert to (mp3, m4a, opus, vorbis, flac)")
			// Output options
			("output,o", po::value<std::string>(),
			 "Output filename template (e.g., %(title)s.%(ext)s)")
			("paths,P", po::value<std::string>(),
			 "Output path for downloads")
			("merge-output-format", po::value<std::string>(),
			 "Container format for merging (mkv, mp4, webm)")
			// Display options
			("dump-json,j", "Output video info as JSON")
			("get-url,g", "Print download URL(s)")
			("print,O", po::value<std::string>(),
			 "Print template field (e.g., %(title)s|%(id)s)")
			("quiet,q", "Suppress output")
			("simulate,s", "Don't download, just print info")
			// Playlist options
			("flat-playlist", "Don't extract each video in playlists")
			// Other
			("stream-audio", "Stream decoded audio to stdout")
			("manual-merge", po::value<std::vector<std::string>>()->multitoken(),
			 "Manually merge: --manual-merge <video> <audio> <output>")
			("verbose,v", "Enable verbose logging");
		// clang-format on

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

		// Handle manual merge (sync operation, no async needed)
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

		if (!vm.count("url")) {
			std::cout << "Usage: yt-dlpp [options] <url>\n" << desc << "\n";
			return 1;
		}

		// Build CLI options
		CliOptions opts;
		opts.url = vm["url"].as<std::string>();
		opts.format = vm["format"].as<std::string>();
		if (vm.count("merge-output-format")) {
			opts.merge_format = vm["merge-output-format"].as<std::string>();
		}
		if (vm.count("output")) {
			opts.output_template = vm["output"].as<std::string>();
		}
		if (vm.count("paths")) {
			opts.output_path = vm["paths"].as<std::string>();
		}
		if (vm.count("print")) {
			opts.print_template = vm["print"].as<std::string>();
		}
		if (vm.count("audio-format")) {
			opts.audio_format = vm["audio-format"].as<std::string>();
		}

		opts.extract_audio = vm.count("extract-audio") > 0;
		opts.quiet = vm.count("quiet") > 0;
		opts.simulate = vm.count("simulate") > 0;
		opts.list_formats = vm.count("list-formats") > 0;
		opts.dump_json = vm.count("dump-json") > 0;
		opts.get_url = vm.count("get-url") > 0;
		opts.stream_audio = vm.count("stream-audio") > 0;
		opts.verbose = vm.count("verbose") > 0;
		opts.flat_playlist = vm.count("flat-playlist") > 0;

		// Auto-select bestaudio format when extracting audio
		if (opts.extract_audio && opts.format == "best") {
			opts.format = "bestaudio";
		}

		// Setup async context
		asio::io_context ioc;
		auto http =
			std::make_shared<ytdlpp::net::HttpClient>(ioc.get_executor());

		// Setup signal handling using asio::signal_set
		asio::signal_set signals(ioc, SIGINT, SIGTERM);
		signals.async_wait([&](const boost::system::error_code &ec, int sig) {
			if (!ec) {
				g_cancelled.store(true);
				fmt::println(
					stderr, "\nExiting normally, received signal {}.", sig);
				ioc.stop();
			}
		});

		// Run the app in a coroutine
		boost::asio::spawn(
			ioc,
			[&](asio::yield_context yield) {
				run_app(ioc, http, opts, std::move(yield));
				// Cancel signal wait so io_context can exit normally
				signals.cancel();
			},
			boost::coroutines::attributes());

		ioc.run();

		return 0;

	} catch (const std::exception &e) {
		fmt::println(stderr, "ERROR: {}", e.what());
		return 1;
	}
}
