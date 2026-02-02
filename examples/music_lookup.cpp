#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

// Include the fix for boost::process::v2 utf8 issue on Windows
#ifdef _WIN32
#include "../tests/fixed_utf8.hpp"
#endif

#include <boost/asio.hpp>
#include <boost/process/v2.hpp>
#include <iostream>
#include <ytdlpp/extractor.hpp>
#include <ytdlpp/http_client.hpp>
#include <ytdlpp/types.hpp>

namespace bp2 = boost::process::v2;
using namespace ytdlpp;

/**
 * Example program to test YouTube Music search and stream validity by
 * downloading a fragment.
 */
int main(int argc, char *argv[]) {
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

	std::string query = "darling, I by Tyler, the Creator";
	if (argc > 1) {
		query = argv[1];
		// If query starts with ytmsearch:, strip it as we add it back
		if (query.find("ytmsearch1:") == 0) {
			query = query.substr(11);
		} else if (query.find("ytmsearch:") == 0) {
			query = query.substr(10);
		}
	}

	// Use ytmsearch1: to get only the top result from YouTube Music
	std::string search_url = "ytmsearch1:" + query;

	std::cout << "YouTube Music Lookup: \"" << query << "\"\n";

	auto opts = youtube::parse_search_url(search_url);
	if (!opts) {
		std::cerr << "Failed to parse search URL\n";
		return 1;
	}

	extractor->async_search(*opts, [&](Result<std::vector<SearchResult>> res) {
		if (res && !res.value().empty()) {
			const auto &first_result = res.value()[0];
			std::cout << "Found result: " << first_result.title << " ("
					  << first_result.video_id << ")\n";

			// Now extract the full info for this video to get the audio URL
			extractor->async_process(first_result.url, [&](Result<VideoInfo>
															   video_res) {
				if (video_res) {
					const auto &info = video_res.value();
					std::cout << "\nMetadata Extracted:\n";
					std::cout << "Title: " << info.title << "\n";
					std::cout << "Uploader: " << info.uploader << "\n";

					// List all available audio formats for debugging
					std::cout << "\nAvailable Audio Formats:\n";
					for (const auto &f : info.formats) {
						if (f.acodec != "none") {
							std::cout
								<< " - ID: " << f.format_id
								<< ", itag: " << f.itag << ", ext: " << f.ext
								<< ", abr: " << f.abr << "k\n";
						}
					}

					// Find the best audio format - prefer itag 251 (Opus)
					// Priority: vr > tv > music
					const VideoFormat *best_audio = nullptr;
					for (const auto &f : info.formats) {
						if (f.acodec != "none") {
							bool is_opus = f.itag == 251;
							bool has_vr =
								f.format_id.find("vr") != std::string::npos;
							bool has_tv =
								f.format_id.find("tv") != std::string::npos;
							bool has_music =
								f.format_id.find("music") != std::string::npos;

							if (!best_audio) {
								best_audio = &f;
								continue;
							}

							// Decision matrix:
							// 1. Prefer Opus (251)
							// 2. Among Opus, prefer vr > tv > music
							// 3. Among same client type, prefer higher bitrate

							bool best_is_opus = best_audio->itag == 251;
							bool best_has_vr =
								best_audio->format_id.find("vr") !=
								std::string::npos;
							bool best_has_tv =
								best_audio->format_id.find("tv") !=
								std::string::npos;
							bool best_has_music =
								best_audio->format_id.find("music") !=
								std::string::npos;

							auto get_client_priority =
								[&](bool vr, bool tv, bool music) {
									if (vr) return 3;
									if (tv) return 2;
									if (music) return 1;
									return 0;
								};

							int current_pri =
								get_client_priority(has_vr, has_tv, has_music);
							int best_pri = get_client_priority(
								best_has_vr, best_has_tv, best_has_music);

							if (is_opus && !best_is_opus) {
								best_audio = &f;
							} else if (is_opus == best_is_opus) {
								if (current_pri > best_pri) {
									best_audio = &f;
								} else if (current_pri == best_pri &&
										   f.abr > best_audio->abr) {
									best_audio = &f;
								}
							}
						}
					}

					if (best_audio) {
						std::cout << "\nChosen Audio Format:\n";
						std::cout << "ID: " << best_audio->format_id
								  << " (itag " << best_audio->itag << ")\n";
						std::cout << "Codec: " << best_audio->acodec << "\n";
						std::cout
							<< "Bitrate: " << best_audio->abr << " kbps\n";
						std::cout << "Headers sent:\n";
						for (const auto &[k, v] : best_audio->http_headers) {
							std::cout << "  " << k << ": " << v << "\n";
						}

						std::string library_url = best_audio->url;
						std::cout << "Library URL: " << library_url << "\n";

						// 1. Get URL from official yt-dlp to ensure parity
						std::cout << "Fetching parity URL from yt-dlp...\n";
						try {
							auto exe =
								bp2::environment::find_executable("yt-dlp");
							std::vector<std::string> args = {
								"-g", "-f", "bestaudio",
								"https://music.youtube.com/watch?v=" + info.id};

							bp2::process proc(ioc, exe, args);
							proc.wait();
							std::cout << "yt-dlp call completed.\n";
						} catch (const std::exception &e) {
							std::cerr
								<< "Failed to run yt-dlp: " << e.what() << "\n";
						}

						// 2. Download a fragment to ensure the stream works
						std::string output_file =
							"test_fragment." + (best_audio->container.empty()
													? "bin"
													: best_audio->container);
						std::cout << "Downloading fragment to: " << output_file
								  << "...\n";

						// We'll download the first 512KB to verify it works
						http->async_download_file(
							library_url, output_file,
							[](long long dl_now, long long dl_total) {
								if (dl_total > 0) {
									std::cout
										<< "\rDownload progress: " << dl_now
										<< " / " << dl_total << " bytes"
										<< std::flush;
								}
							},
							[http, work_guard, output_file,
							 &ioc](Result<void> dl_res) mutable {
								std::cout << "\n";
								if (dl_res) {
									std::cout << "SUCCESS: Fragment downloaded "
												 "successfully to "
											  << output_file << "\n";
								} else {
									std::cout
										<< "ERROR: Failed to download "
										   "fragment: "
										<< dl_res.error().message() << "\n";
								}
								// Stop the io_context after the download
								// attempt
								std::cout << "Stopping ioc...\n";
								ioc.stop();
							},
							best_audio->http_headers);

					} else {
						std::cerr << "No audio-only format found.\n";
						work_guard.reset();
					}
				} else {
					std::cerr << "Detailed extraction failed: "
							  << video_res.error().message() << "\n";
					work_guard.reset();
				}
			});
		} else {
			if (res.has_error()) {
				std::cerr << "Search failed: " << res.error().message() << "\n";
			} else {
				std::cerr << "No results found.\n";
			}
			work_guard.reset();
		}
	});

	ioc.run();
	std::cout << "Program exited successfully.\n";
	return 0;
}
