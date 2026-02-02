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
#include <ytdlpp/cookie_jar.hpp>
#include <ytdlpp/downloader.hpp>
#include <ytdlpp/extractor.hpp>
#include <ytdlpp/extractor_args.hpp>
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
	// Sort formats: resolution (ascending), then tbr (ascending)
	std::sort(formats.begin(), formats.end(), [](const auto &a, const auto &b) {
		// Calculate resolution area for sorting
		long long area_a = (long long)a.width * a.height;
		long long area_b = (long long)b.width * b.height;

		if (area_a != area_b) return area_a < area_b;
		return a.tbr < b.tbr;
	});

	// Build table data first to calculate column widths
	struct Row {
		std::string id, ext, res, fps, size, tbr, proto, vcodec, vbr, acodec,
			abr, asr, info;
		bool is_grey = false;
	};
	std::vector<Row> rows;

	constexpr double MIB = 1048576.0;  // 1024 * 1024

	for (const auto &f : formats) {
		Row r;
		r.id = f.format_id.empty() ? std::to_string(f.itag) : f.format_id;
		r.ext = f.ext.empty() ? "unk" : f.ext;

		// Resolution logic
		if (f.vcodec == "none") {
			r.res = "audio only";
		} else if (f.width > 0) {
			r.res = fmt::format("{}x{}", f.width, f.height);
		} else {
			r.res = "images";  // or "unknown"
		}

		r.fps = (f.fps > 0) ? std::to_string(f.fps) : "";

		// File size
		if (f.filesize_approx > 0) {
			double mib = static_cast<double>(f.filesize_approx) / MIB;
			r.size = fmt::format("~{:.2f}MiB", mib);
		} else if (f.content_length > 0) {
			double mib = static_cast<double>(f.content_length) / MIB;
			r.size = fmt::format("{:.2f}MiB", mib);
		} else {
			r.size = "~";
		}

		// TBR (Total Bit Rate) - already in kbps from extractor
		r.tbr = (f.tbr > 0) ? fmt::format("{:.0f}k", f.tbr) : "";

		r.proto = f.protocol.empty() ? "https" : f.protocol;
		if (r.proto == "http_dash_segments") r.proto = "dash";
		if (r.proto == "m3u8_native") r.proto = "m3u8";

		// VCodec
		if (f.vcodec == "none") {
			r.vcodec = "audio only";
			r.vbr = "";
		} else {
			r.vcodec = f.vcodec.empty() ? "unknown" : f.vcodec.substr(0, 12);
			r.vbr = (f.vbr > 0) ? fmt::format("{:.0f}k", f.vbr) : "";
		}

		// ACodec
		if (f.acodec == "none") {
			r.acodec = "video only";
			r.abr = "";
			r.asr = "";
		} else {
			r.acodec = f.acodec.empty() ? "unknown" : f.acodec.substr(0, 12);
			r.abr = (f.abr > 0) ? fmt::format("{:.0f}k", f.abr) : "";
			r.asr = (f.audio_sample_rate > 0)
						? fmt::format("{}Hz", f.audio_sample_rate)
						: "";
		}

		r.info = f.format_note;
		if (f.acodec != "none" && f.vcodec == "none") {
			// Add audio specific info if available
			if (f.container == "webm_dash") r.info += ", webm_dash";
			if (f.container == "m4a_dash") r.info += ", m4a_dash";
		}

		// Grey out if only video or only audio (optional, but yt-dlp does
		// colourizing differently) We'll keep the grey logic for streams that
		// aren't "complete" (both audio+video), but typically yt-dlp doesn't
		// grey out rows, it just colors specific columns. For now, let's just
		// make video-only or audio-only simpler.
		r.is_grey = (f.vcodec == "none" || f.acodec == "none");

		// If we are printing to console, we want to align roughly with yt-dlp
		rows.push_back(r);
	}

	// Print header
	// yt-dlp format:
	// ID  EXT   RESOLUTION FPS │   FILESIZE   TBR PROTO │ VCODEC          VBR
	// ACODEC      ABR ASR MORE INFO

	fmt::println(
		"{:<6} {:<5} {:<11} {:<3} | {:>10} {:>5} {:<6} | {:<12} {:>4} {:<12} "
		"{:>4} {:>6} {}",
		"ID", "EXT", "RESOLUTION", "FPS", "FILESIZE", "TBR", "PROTO", "VCODEC",
		"VBR", "ACODEC", "ABR", "ASR", "MORE INFO");

	for (const auto &r : rows) {
		std::string row_str = fmt::format(
			"{:<6} {:<5} {:<11} {:<3} | {:>10} {:>5} {:<6} | {:<12} {:>4} "
			"{:<12} {:>4} {:>6} {}",
			r.id, r.ext, r.res, r.fps, r.size, r.tbr, r.proto, r.vcodec, r.vbr,
			r.acodec, r.abr, r.asr, r.info);

		if (r.is_grey) {
			fmt::print(fg(fmt::color::dim_gray), "{}\n", row_str);
		} else {
			fmt::println("{}", row_str);
		}
	}
}

// =============================================================================
// yt-dlp Compatible Output Formatting
// =============================================================================

// Global flag for quiet mode
static bool g_quiet = false;

// Log with proper prefix like yt-dlp
// Patterns: [youtube:search], [youtube], [download], [info]
void log_search(std::string_view msg) {
	if (!g_quiet) fmt::println(stderr, "[youtube:search] {}", msg);
}

void log_download(std::string_view msg) {
	if (!g_quiet) fmt::println(stderr, "[download] {}", msg);
}

void log_youtube(std::string_view msg) {
	if (!g_quiet) fmt::println(stderr, "[youtube] {}", msg);
}

void log_info(std::string_view msg) {
	if (!g_quiet) fmt::println(stderr, "[info] {}", msg);
}

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

	// Audio track selection
	std::optional<std::string> audio_lang;	// --audio-lang (e.g., "ja", "en")
	std::string format_sort;				// --format-sort, -S

	// New: display options
	bool quiet = false;
	bool simulate = false;
	std::string print_template;	 // -O print template

	// Simple info getters
	bool get_title = false;
	bool get_id = false;
	bool get_thumbnail = false;
	bool get_description = false;
	bool get_filename = false;

	// Existing options
	bool list_formats = false;
	bool dump_json = false;
	bool get_url = false;
	bool stream_audio = false;
	bool verbose = false;
	bool flat_playlist = false;

	// Extractor arguments (e.g., "youtube:player_client=web,-android_sdkless")
	std::optional<std::string> extractor_args;

	// Overwrite options
	bool force_overwrites = false;
	bool no_overwrites = false;
};

// Main application logic using yield_context for clean async
bool run_app(asio::io_context &ioc,
			 const std::shared_ptr<ytdlpp::net::HttpClient> &http,
			 const CliOptions &opts, asio::yield_context yield) {
	bool success = true;
	ytdlpp::youtube::Extractor extractor(http, ioc.get_executor());

	// Apply extractor arguments if provided
	if (opts.extractor_args) {
		ytdlpp::youtube::ExtractorArgs args(*opts.extractor_args);
		extractor.set_extractor_args(args);
		spdlog::debug("Applied extractor args: {}", *opts.extractor_args);
	}

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
			return false;
		}

		const auto &results = search_result.value();
		log_search(fmt::format(
			"Playlist {}: Downloading {} items of {}", search_opts->query,
			results.size(), results.size()));

		if (results.empty()) {
			fmt::println(stderr, "ERROR: No results found for: \"{}\"",
						 search_opts->query);
			return false;
		}

		// If --dump-json, output results as JSON
		if (opts.dump_json) {
			nlohmann::json j = nlohmann::json::array();
			for (const auto &r : results) {
				nlohmann::json item = r;
				j.push_back(item);
			}
			std::cout << j.dump(2) << "\n";
			return true;
		}

		// For --flat-playlist, just list items
		if (opts.flat_playlist) {
			for (size_t i = 0; i < results.size(); ++i) {
				log_download(fmt::format(
					"Downloading item {} of {}", i + 1, results.size()));
			}
			log_download(fmt::format(
				"Finished downloading playlist: {}", search_opts->query));
			return true;
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

			auto info = info_result.value();
			info.playlist_index = static_cast<int>(i + 1);
			if (search_opts) {
				info.playlist_title = search_opts->query;  // Fallback
			}

			// Handle simple info getters
			bool any_get =
				opts.get_url || opts.get_title || opts.get_id ||
				opts.get_thumbnail || opts.get_description ||
				opts.get_filename || opts.list_formats || opts.dump_json;

			if (any_get) {
				if (opts.dump_json) {
					std::cout << nlohmann::json(info).dump() << "\n";
				}
				if (opts.list_formats) { print_formats_table(info.formats); }
				if (opts.get_title) std::cout << info.title << "\n";
				if (opts.get_id) std::cout << info.id << "\n";
				if (opts.get_thumbnail) std::cout << info.thumbnail << "\n";
				if (opts.get_description) std::cout << info.description << "\n";
				if (opts.get_filename) {
					auto streams = ytdlpp::Downloader::select_streams(
						info, opts.format, opts.audio_lang, opts.format_sort);
					const auto *v = streams.video;
					const auto *a = streams.audio;

					// Create a mutable copy for template expansion
					ytdlpp::VideoInfo temp_info = info;

					if (v && a && v != a) {
						temp_info.ext = v->ext;
						temp_info.format_id = v->format_id + "+" + a->format_id;
					} else if (v) {
						temp_info.ext = v->ext;
						temp_info.format_id = v->format_id;
					} else if (a) {
						temp_info.ext = a->ext;
						temp_info.format_id = a->format_id;
					}
					std::cout
						<< ytdlpp::sanitize_path(ytdlpp::expand_output_template(
							   opts.output_template, temp_info))
						<< "\n";
				}
				if (opts.get_url) {
					auto streams = ytdlpp::Downloader::select_streams(
						info, opts.format, opts.audio_lang, opts.format_sort);
					if (streams.video) {
						std::cout << streams.video->url << "\n";
					}
					if (streams.audio && streams.audio != streams.video) {
						std::cout << streams.audio->url << "\n";
					}
				}
				continue;  // Process next search result
			}

			// Select format and show info line
			auto streams = ytdlpp::Downloader::select_streams(
				info, opts.format, opts.audio_lang, opts.format_sort);
			if (!streams.video && !streams.audio) {
				fmt::println(
					stderr, "ERROR: No matching format for: {}", opts.format);
				continue;
			}

			std::string format_str;
			if (streams.video && streams.audio &&
				streams.video != streams.audio) {
				format_str = std::to_string(streams.video->itag) + "+" +
							 std::to_string(streams.audio->itag);
			} else if (streams.video) {
				format_str = std::to_string(streams.video->itag);
			} else if (streams.audio) {
				format_str = std::to_string(streams.audio->itag);
			}

			if (!opts.quiet) {
				log_info(fmt::format(
					"{}: Downloading 1 format(s): {}", info.id, format_str));
			}

			// Handle --simulate (don't download)
			if (opts.simulate) { continue; }

			// Otherwise download
			// Otherwise download
			ytdlpp::Downloader downloader(http);

			ytdlpp::Downloader::DownloadOptions dl_opts;
			dl_opts.format_selector = opts.format;
			dl_opts.merge_format = opts.merge_format;
			dl_opts.output_template = opts.output_template;
			dl_opts.force_overwrites = opts.force_overwrites;
			dl_opts.no_overwrites = opts.no_overwrites;

			auto download_result = downloader.async_download(
				info, std::move(dl_opts),
				[&opts](const std::string &status,
						const ytdlpp::DownloadProgress &prog) {
					if (!opts.quiet) {
						std::cout << "\r" << status << ": " << prog.percentage
								  << "%   " << std::flush;
					}
				},
				yield);

			if (download_result.has_error()) {
				fmt::println(stderr, "\nERROR: Download failed: {}",
							 download_result.error().message());
			} else {
				if (!opts.quiet) {
					// Use the actual filename returned by result
					fmt::println(
						"\n[download] 100%% of {}", download_result.value());
				}
			}
		}

		log_download(fmt::format(
			"Finished downloading playlist: {}", search_opts->query));
		return success;
	}

	// Regular video extraction
	log_youtube(fmt::format("Extracting URL: {}", opts.url));

	auto info_result = extractor.async_process(opts.url, yield);
	if (info_result.has_error()) {
		fmt::println(stderr, "ERROR: Failed to extract info: {}",
					 info_result.error().message());
		return false;
	}

	auto info = info_result.value();

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
			return false;
		}

		ytdlpp::media::AudioStreamOptions audio_opts;
		audio_opts.sample_rate = 48000;
		audio_opts.channels = 2;
		audio_opts.sample_fmt = ytdlpp::media::SampleFormat::S16;

		// Inject cookies and headers from HttpClient
		std::string cookie_header =
			http->get_cookie_jar().build_cookie_header();
		if (!cookie_header.empty()) {
			audio_opts.headers["Cookie"] = cookie_header;
		}
		audio_opts.headers["User-Agent"] =
			"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
			"(KHTML, like Gecko) Chrome/121.0.0.0 Safari/537.36";
		audio_opts.headers["Referer"] = "https://www.youtube.com/";

		ytdlpp::media::AudioStreamer streamer(ioc.get_executor());
		auto stream = streamer.async_open(best->url, audio_opts, yield);
		if (stream.has_error()) {
			spdlog::error(
				"Failed to open stream: {}", stream.error().message());
			return false;
		}

		auto &audio_stream = stream.value();
		while (!audio_stream.is_eof() && !g_cancelled.load()) {
			auto read_result = audio_stream.async_read(yield);
			if (read_result.has_error() || read_result.value().empty()) break;
			const auto &audio_data = read_result.value();
			std::fwrite(audio_data.data(), 1, audio_data.size(), stdout);
		}
		return true;
	}

	// Handle --dump-json
	if (opts.dump_json) {
		// Select best format to populate top-level fields (like yt-dlp)
		auto streams = ytdlpp::Downloader::select_streams(
			info, opts.format, opts.audio_lang, opts.format_sort);

		const ytdlpp::VideoFormat *v = streams.video;
		const ytdlpp::VideoFormat *a = streams.audio;

		if (v && a && v != a) {
			info.format_id = v->format_id + "+" + a->format_id;
			info.ext = v->ext;	// Video extension usually wins? yt-dlp might
								// merge to mkv/mp4
			info.format = v->format_note + " + " + a->format_note;
			info.resolution = fmt::format("{}x{}", v->width, v->height);
		} else if (v) {
			info.format_id = v->format_id;
			info.ext = v->ext;
			info.format = v->format_note;
			info.resolution = fmt::format("{}x{}", v->width, v->height);
		} else if (a) {
			info.format_id = a->format_id;
			info.ext = a->ext;
			info.format = a->format_note;
			info.resolution = "audio only";
		}

		nlohmann::json j = info;

		// Merge selected format fields into top-level JSON
		auto merge_field = [&](const std::string &key, const auto &val) {
			j[key] = val;
		};

		if (v && a && v != a) {
			nlohmann::json req_formats = nlohmann::json::array();

			auto add_fmt = [&](const ytdlpp::VideoFormat &f) {
				nlohmann::json item;
				item["format_id"] =
					f.format_id.empty() ? std::to_string(f.itag) : f.format_id;
				item["url"] = f.url;

				// Standard fields
				item["ext"] = f.ext.empty() ? "unknown" : f.ext;
				if (f.width > 0) item["width"] = f.width;
				if (f.height > 0) item["height"] = f.height;
				if (f.fps > 0) item["fps"] = f.fps;
				item["protocol"] = f.protocol;
				item["vcodec"] = f.vcodec;
				item["acodec"] = f.acodec;
				if (f.tbr > 0) item["tbr"] = f.tbr;
				if (f.filesize_approx > 0) item["filesize"] = f.filesize_approx;

				req_formats.push_back(item);
			};

			add_fmt(*v);
			add_fmt(*a);
			j["requested_formats"] = req_formats;

			// yt-dlp removes top-level URL for merged formats
			if (j.contains("url")) j.erase("url");
		}

		if (v) {
			if (v->width > 0) merge_field("width", v->width);
			if (v->height > 0) merge_field("height", v->height);
			if (v->fps > 0) merge_field("fps", v->fps);
			if (v->vcodec != "none") merge_field("vcodec", v->vcodec);
			if (v->vbr > 0) merge_field("vbr", v->vbr);
			if (!v->dynamic_range.empty())
				merge_field("dynamic_range", v->dynamic_range);
			if (v->aspect_ratio > 0)
				merge_field("aspect_ratio", v->aspect_ratio);
		}

		if (a) {
			if (a->acodec != "none") merge_field("acodec", a->acodec);
			if (a->abr > 0) merge_field("abr", a->abr);
			if (a->audio_sample_rate > 0)
				merge_field("asr", a->audio_sample_rate);
			if (a->audio_channels > 0)
				merge_field("audio_channels", a->audio_channels);
		}

		// Common fields / overrides
		double tbr = 0;
		if (v) tbr += v->tbr;
		if (a && a != v) tbr += a->tbr;
		if (tbr > 0) merge_field("tbr", tbr);

		// Protocol
		if (v && a && v != a) {
			// merged
			merge_field("protocol", "https+https");	 // simplified
		} else if (v) {
			merge_field("protocol", v->protocol);
		} else if (a) {
			merge_field("protocol", a->protocol);
		}

		std::cout << j.dump(2) << "\n";
		return true;
	}

	// Handle --list-formats
	if (opts.list_formats) {
		spdlog::debug(
			"print_formats_table called with {} formats", info.formats.size());
		print_formats_table(info.formats);
		return true;
	}

	// Handle -O, --print (template printing)
	if (!opts.print_template.empty()) {
		auto output = ytdlpp::expand_output_template(
			opts.print_template, info, "", false);
		std::cout << output << "\n";
		return true;
	}

	// Handle simple info getters
	bool any_get =
		opts.get_url || opts.get_title || opts.get_id || opts.get_thumbnail ||
		opts.get_description || opts.get_filename;

	if (any_get) {
		if (opts.get_title) std::cout << info.title << "\n";
		if (opts.get_id) std::cout << info.id << "\n";
		if (opts.get_thumbnail) std::cout << info.thumbnail << "\n";
		if (opts.get_description) std::cout << info.description << "\n";
		if (opts.get_filename) {
			auto streams = ytdlpp::Downloader::select_streams(
				info, opts.format, opts.audio_lang, opts.format_sort);
			const auto *v = streams.video;
			const auto *a = streams.audio;

			// Create a mutable copy for template expansion
			ytdlpp::VideoInfo temp_info = info;

			if (v && a && v != a) {
				temp_info.ext = v->ext;
				temp_info.format_id = v->format_id + "+" + a->format_id;
			} else if (v) {
				temp_info.ext = v->ext;
				temp_info.format_id = v->format_id;
			} else if (a) {
				temp_info.ext = a->ext;
				temp_info.format_id = a->format_id;
			}
			std::cout << ytdlpp::sanitize_path(ytdlpp::expand_output_template(
							 opts.output_template, temp_info))
					  << "\n";
		}
		if (opts.get_url) {
			auto streams = ytdlpp::Downloader::select_streams(
				info, opts.format, opts.audio_lang, opts.format_sort);
			if (streams.video) { std::cout << streams.video->url << "\n"; }
			if (streams.audio && streams.audio != streams.video) {
				std::cout << streams.audio->url << "\n";
			}
		}
		return true;
	}
	// Select format and show info (like yt-dlp)
	auto streams = ytdlpp::Downloader::select_streams(
		info, opts.format, opts.audio_lang, opts.format_sort);
	if (!streams.video && !streams.audio) {
		fmt::println(
			stderr, "ERROR: No matching format found for: {}", opts.format);
		return false;
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
	if (opts.simulate) { return true; }

	// Download mode
	ytdlpp::Downloader downloader(http);

	ytdlpp::Downloader::DownloadOptions dl_opts;
	dl_opts.format_selector = opts.format;
	dl_opts.merge_format = opts.merge_format;
	dl_opts.output_template = opts.output_template;
	dl_opts.force_overwrites = opts.force_overwrites;
	dl_opts.no_overwrites = opts.no_overwrites;

	auto download_result = downloader.async_download(
		info, std::move(dl_opts),
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
		return false;
	} else {
		if (!opts.quiet) {
			fmt::println("\n[download] 100%% of {}", download_result.value());
		}
		return true;
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
			("format-sort,S", po::value<std::string>(),
			 "Sort the formats by the fields given (e.g. res,fps)")
			("list-formats,F", "List available formats")
			// Audio extraction
			("extract-audio,x", "Convert video to audio-only file")
			("audio-format", po::value<std::string>(),
			 "Audio format to convert to (mp3, m4a, opus, vorbis, flac)")
			("audio-lang", po::value<std::string>(),
			 "Prefer audio with this language code (e.g., ja, en, es)")
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
			// Simple info getters
			("get-title,e", "Print title")
			("get-id", "Print video ID")
			("get-thumbnail", "Print thumbnail URL")
			("get-description", "Print video description")
			("get-filename", "Print output filename")
			("quiet,q", "Suppress output")
			("simulate,s", "Don't download, just print info")
			// Playlist options
			("flat-playlist", "Don't extract each video in playlists")
			// Other
			("stream-audio", "Stream decoded audio to stdout")
			("manual-merge", po::value<std::vector<std::string>>()->multitoken(),
			 "Manually merge: --manual-merge <video> <audio> <output>")
			("extractor-args", po::value<std::string>(),
			 "Extractor arguments (e.g., \"youtube:player_client=web,-android_sdkless\")")
			("verbose,v", "Enable verbose logging")
			// Overwrite behavior
			("force-overwrites", "Overwrite matching files")
			("no-overwrites", "Do not overwrite matching files (default)");
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
		if (vm.count("format-sort")) {
			opts.format_sort = vm["format-sort"].as<std::string>();
		}
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

		opts.get_title = vm.count("get-title") > 0;
		opts.get_id = vm.count("get-id") > 0;
		opts.get_thumbnail = vm.count("get-thumbnail") > 0;
		opts.get_description = vm.count("get-description") > 0;
		opts.get_filename = vm.count("get-filename") > 0;

		if (vm.count("audio-format")) {
			opts.audio_format = vm["audio-format"].as<std::string>();
		}
		if (vm.count("audio-lang")) {
			opts.audio_lang = vm["audio-lang"].as<std::string>();
		}

		opts.extract_audio = vm.count("extract-audio") > 0;
		opts.simulate = vm.count("simulate") > 0;
		opts.list_formats = vm.count("list-formats") > 0;
		opts.dump_json = vm.count("dump-json") > 0;
		opts.get_url = vm.count("get-url") > 0;
		opts.stream_audio = vm.count("stream-audio") > 0;
		opts.verbose = vm.count("verbose") > 0;

		// Auto-enable quiet mode if getters are used (like yt-dlp)
		bool any_getter =
			opts.get_url || opts.get_title || opts.get_id ||
			opts.get_thumbnail || opts.get_description || opts.get_filename ||
			opts.dump_json || opts.list_formats || !opts.print_template.empty();

		if (vm.count("quiet")) {
			opts.quiet = true;
		} else if (any_getter) {
			opts.quiet = true;
		} else {
			opts.quiet = false;
		}

		g_quiet = opts.quiet;

		if (opts.quiet) {
			if (opts.verbose) {
				spdlog::set_level(spdlog::level::debug);
			} else {
				spdlog::set_level(spdlog::level::err);
			}
		}
		opts.flat_playlist = vm.count("flat-playlist") > 0;

		if (vm.count("extractor-args")) {
			opts.extractor_args = vm["extractor-args"].as<std::string>();
		}

		opts.force_overwrites = vm.count("force-overwrites") > 0;
		opts.no_overwrites = vm.count("no-overwrites") > 0;

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
		int exit_code = 0;
		boost::asio::spawn(
			ioc,
			[&](asio::yield_context yield) {
				if (!run_app(ioc, http, opts, std::move(yield))) {
					exit_code = 1;
				}
				// Cancel signal wait so io_context can exit normally
				signals.cancel();
			},
			boost::coroutines::attributes());

		ioc.run();

		return exit_code;

	} catch (const std::exception &e) {
		fmt::println(stderr, "ERROR: {}", e.what());
		return 1;
	}
}
