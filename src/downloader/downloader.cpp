#include <spdlog/spdlog.h>

#include <algorithm>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <filesystem>
#include <utility>
#include <ytdlpp/downloader.hpp>
#include <ytdlpp/http_client.hpp>
#include <ytdlpp/output_template.hpp>

namespace fs = std::filesystem;

#include "media/muxer.hpp"

namespace ytdlpp {

namespace {
// (Removed sanitize_filename_local as we use output_template.hpp now)
}  // namespace

// Struct Impl definition
struct Downloader::Impl {
	std::shared_ptr<ytdlpp::net::HttpClient> http;

	explicit Impl(std::shared_ptr<ytdlpp::net::HttpClient> h)
		: http(std::move(h)) {}

	// Logic for stream selection
	static Downloader::StreamInfo select_streams(
		const VideoInfo &info, std::string_view selector,
		std::optional<std::string> preferred_lang = std::nullopt,
		std::string_view sort_order = "");

	// Async download logic delegate
	void async_download(
		const ytdlpp::VideoInfo &info, Downloader::DownloadOptions options,
		ytdlpp::ProgressCallback progress_cb,
		asio::any_completion_handler<void(Result<std::string>)> handler,
		Downloader::CompletionExecutor handler_ex);

   private:
	// Helpers
	static bool merge_streams(const std::string &video_path,
							  const std::string &audio_path,
							  const std::string &output_path);
};

// Session class for managing async requests lifecycle
class AsyncDownloaderSession
	: public std::enable_shared_from_this<AsyncDownloaderSession> {
   public:
	using CompletionExecutor = Downloader::CompletionExecutor;

	AsyncDownloaderSession(
		std::shared_ptr<ytdlpp::net::HttpClient> http,
		asio::any_completion_handler<void(Result<std::string>)> cb,
		CompletionExecutor handler_ex, ProgressCallback progress_cb)
		: http_(std::move(http)),
		  cb_(std::move(cb)),
		  handler_ex_(std::move(handler_ex)),
		  progress_cb_(std::move(progress_cb)) {}

	void start(const VideoInfo &info, Downloader::DownloadOptions options) {
		info_ = info;
		options_ = std::move(options);

		streams_ = Downloader::select_streams(info_, options_.format_selector);

		if (!streams_.video && !streams_.audio) {
			complete(outcome::failure(errc::video_not_found));
			return;
		}

		// Prepare info for template expansion (simulate final result)
		VideoInfo temp_info = info_;
		if (streams_.video && streams_.audio &&
			streams_.video != streams_.audio) {
			// Merged
			temp_info.ext = options_.merge_format.value_or(streams_.video->ext);
			temp_info.format_id =
				streams_.video->format_id + "+" + streams_.audio->format_id;
		} else if (streams_.video) {
			temp_info.ext = streams_.video->ext;
			temp_info.format_id = streams_.video->format_id;
		} else if (streams_.audio) {
			temp_info.ext = streams_.audio->ext;
			temp_info.format_id = streams_.audio->format_id;
		}

		// Generate final output path
		final_output_path_ = sanitize_path(expand_output_template(
			options_.output_template, temp_info, temp_info.ext));

		// Check for existing file
		if (fs::exists(final_output_path_)) {
			if (options_.force_overwrites) {
				spdlog::info("[download] Overwriting existing file: {}",
							 final_output_path_);
			} else {
				// Skip
				spdlog::info("[download] {} has already been downloaded",
							 final_output_path_);
				report_progress("finished");
				complete(outcome::success(final_output_path_));
				return;
			}
		}

		// Calculate intermediate paths
		// If merging, use .f<ID> suffix for parts
		// strip extension from final path to get base
		fs::path final_p(final_output_path_);
		std::string base = final_p.parent_path().string();
		if (!base.empty()) base += "/";
		base += final_p.stem().string();

		if (streams_.video) {
			if (streams_.audio && streams_.video != streams_.audio) {
				// Separate video file
				video_path_ = base + ".f" + streams_.video->format_id + "." +
							  streams_.video->ext;
			} else {
				// Valid single file (video only or combined)
				video_path_ = final_output_path_;
			}
		}

		if (streams_.audio && streams_.audio != streams_.video) {
			audio_path_ = base + ".f" + streams_.audio->format_id + "." +
						  streams_.audio->ext;
		}

		// Calculate total operations
		active_downloads_ = 0;
		if (streams_.video) active_downloads_++;
		if (streams_.audio && streams_.audio != streams_.video)
			active_downloads_++;

		if (streams_.video) download_video();
		if (streams_.audio && streams_.audio != streams_.video)
			download_audio();
	}

   private:
	std::shared_ptr<ytdlpp::net::HttpClient> http_;
	asio::any_completion_handler<void(Result<std::string>)> cb_;
	CompletionExecutor handler_ex_;
	ProgressCallback progress_cb_;
	VideoInfo info_;
	Downloader::DownloadOptions options_;
	Downloader::StreamInfo streams_;
	std::string final_output_path_;
	std::string video_path_;
	std::string audio_path_;
	int active_downloads_ = 0;
	bool error_occurred_ = false;

	// Progress Tracking
	long long total_video_bytes_ = 0;
	long long total_audio_bytes_ = 0;
	long long current_video_bytes_ = 0;
	long long current_audio_bytes_ = 0;
	std::chrono::steady_clock::time_point start_time_;

	void download_video() {
		std::string part_path = video_path_ + ".part";
		spdlog::info("Downloading video: {}", part_path);
		http_->async_download_file(
			streams_.video->url, part_path,
			[this](long long now, long long total) {
				current_video_bytes_ = now;
				if (total > 0) total_video_bytes_ = total;
				report_progress("downloading video");
			},
			[self = shared_from_this(), part_path](Result<void> res) mutable {
				if (res.has_error()) {
					spdlog::error(
						"Video download failed: {}", res.error().message());
					self->error_occurred_ = true;
				} else {
					std::error_code ec;
					if (fs::exists(self->video_path_))
						fs::remove(self->video_path_, ec);
					fs::rename(part_path, self->video_path_, ec);
					if (ec) {
						spdlog::error("Failed to rename video .part file: {}",
									  ec.message());
						self->error_occurred_ = true;
					}
				}
				self->on_download_complete();
			},
			streams_.video->http_headers);
	}

	void download_audio() {
		std::string part_path = audio_path_ + ".part";
		spdlog::info("Downloading audio: {}", part_path);
		http_->async_download_file(
			streams_.audio->url, part_path,
			[this](long long now, long long total) {
				current_audio_bytes_ = now;
				if (total > 0) total_audio_bytes_ = total;
				report_progress("downloading audio");
			},
			[self = shared_from_this(), part_path](Result<void> res) mutable {
				if (res.has_error()) {
					spdlog::error(
						"Audio download failed: {}", res.error().message());
					self->error_occurred_ = true;
				} else {
					std::error_code ec;
					if (fs::exists(self->audio_path_))
						fs::remove(self->audio_path_, ec);
					fs::rename(part_path, self->audio_path_, ec);
					if (ec) {
						spdlog::error("Failed to rename audio .part file: {}",
									  ec.message());
						self->error_occurred_ = true;
					}
				}
				self->on_download_complete();
			},
			streams_.audio->http_headers);
	}

	void report_progress(const std::string &status) {
		if (!progress_cb_) return;

		long long total_current = current_video_bytes_ + current_audio_bytes_;
		long long total_size = total_video_bytes_ + total_audio_bytes_;

		// Initialize start time on first bytes
		if (start_time_.time_since_epoch().count() == 0 && total_current > 0) {
			start_time_ = std::chrono::steady_clock::now();
		}

		DownloadProgress prog{};
		prog.total_downloaded_bytes = total_current;
		prog.total_size_bytes = total_size;

		if (total_size > 0) {
			prog.percentage = static_cast<double>(total_current) /
							  static_cast<double>(total_size) * 100.0;
		}

		if (start_time_.time_since_epoch().count() > 0) {
			auto now = std::chrono::steady_clock::now();
			auto duration =
				std::chrono::duration_cast<std::chrono::milliseconds>(
					now - start_time_)
					.count();

			if (duration > 0) {
				constexpr double kMsPerSec = 1000.0;
				prog.speed_bytes_per_sec =
					static_cast<double>(total_current) * kMsPerSec /
					static_cast<double>(duration);

				if (prog.speed_bytes_per_sec > 0 && total_size > 0) {
					long long remaining = total_size - total_current;
					prog.eta_seconds = static_cast<double>(remaining) /
									   prog.speed_bytes_per_sec;
				}
			}
		}

		progress_cb_(status, prog);
	}

	void on_download_complete() {
		if (--active_downloads_ == 0) {
			if (error_occurred_) {
				complete(outcome::failure(errc::request_failed));
				return;
			}
			finalize();
		}
	}

	void finalize() {
		if (streams_.video && streams_.audio &&
			streams_.video != streams_.audio) {
			spdlog::info("Merging video and audio...");
			report_progress("merging");

			if (media::Muxer::merge(
					video_path_, audio_path_, final_output_path_)) {
				std::filesystem::remove(video_path_);
				std::filesystem::remove(audio_path_);
				complete(outcome::success(final_output_path_));
				return;
			}
			spdlog::error("Merge failed");
			complete(outcome::failure(errc::muxer_error));
			return;
		}

		// If single file, it's already in the final path (see logic in start())
		// unless we want to support .part files later.
		complete(outcome::success(final_output_path_));
		return;
	}

	void complete(Result<std::string> res) {
		asio::dispatch(
			handler_ex_, [cb = std::move(cb_), res = std::move(res)]() mutable {
				cb(std::move(res));
			});
	}
};

void Downloader::Impl::async_download(
	const ytdlpp::VideoInfo &info, DownloadOptions options,
	ytdlpp::ProgressCallback progress_cb,
	asio::any_completion_handler<void(Result<std::string>)> handler,
	Downloader::CompletionExecutor handler_ex) {
	std::make_shared<AsyncDownloaderSession>(
		http, std::move(handler), std::move(handler_ex), std::move(progress_cb))
		->start(info, std::move(options));
}

Downloader::StreamInfo Downloader::Impl::select_streams(
	const VideoInfo &info, std::string_view selector,
	std::optional<std::string> preferred_lang, std::string_view sort_order) {
	StreamInfo result;

	// Handle merge selectors (e.g. "bestvideo+bestaudio")
	size_t plus = selector.find('+');
	if (plus != std::string_view::npos) {
		auto left = selector.substr(0, plus);
		auto right = selector.substr(plus + 1);
		StreamInfo s1 = select_streams(info, left, preferred_lang);
		StreamInfo s2 = select_streams(info, right, preferred_lang);

		result.video = s1.video ? s1.video : s2.video;
		result.audio = s1.audio ? s1.audio : s2.audio;
		return result;
	}

	// Selector parsing
	std::string base_selector = std::string(selector);
	std::vector<std::pair<std::string, std::string>> filters;

	// Extract [key=value] filters
	size_t bracket_start = base_selector.find('[');
	while (bracket_start != std::string::npos) {
		size_t bracket_end = base_selector.find(']', bracket_start);
		if (bracket_end == std::string::npos) break;

		std::string filter = base_selector.substr(
			bracket_start + 1, bracket_end - bracket_start - 1);

		size_t equals = filter.find('=');
		if (equals != std::string::npos) {
			std::string key = filter.substr(0, equals);
			std::string val = filter.substr(equals + 1);
			filters.emplace_back(key, val);
		}

		bracket_start = base_selector.find('[', bracket_end);
	}

	// Clean base selector (remove anything after first [)
	size_t first_bracket = base_selector.find('[');
	if (first_bracket != std::string::npos) {
		base_selector = base_selector.substr(0, first_bracket);
	}

	bool is_worst = base_selector.find("worst") != std::string::npos;
	bool is_audio_only = base_selector.find("audio") != std::string::npos;
	bool is_video_only = base_selector.find("video") != std::string::npos;

	// Filter validation lambda
	auto passes_filters = [&](const VideoFormat &f) -> bool {
		for (const auto &[key, val] : filters) {
			try {
				if (key == "height") {
					int h = std::stoi(val);
					if (f.height != h) return false;
				} else if (key == "width") {
					int w = std::stoi(val);
					if (f.width != w) return false;
				} else if (key == "ext") {
					if (f.ext != val) return false;
				} else if (key == "fps") {
					int fps = std::stoi(val);
					if (f.fps != fps) return false;
				} else if (key == "vcodec") {
					// Simple substring match for now, or strict?
					// yt-dlp "vcodec=av01" matches "av01..."
					if (val == "none") {
						if (f.vcodec != "none") return false;
					} else {
						if (f.vcodec.find(val) == std::string::npos)
							return false;
					}
				} else if (key == "acodec") {
					if (val == "none") {
						if (f.acodec != "none") return false;
					} else {
						if (f.acodec.find(val) == std::string::npos)
							return false;
					}
				}
			} catch (...) {
				return false;  // atoi failure
			}
		}
		return true;
	};

	// Heuristics
	// Returns true if 'f' is "better" than 'base' (where "better" depends on
	// direction)
	// HEURISTICS DEFINITIONS
	auto get_vcodec_score = [](const std::string &codec) -> int {
		if (codec.find("av01") != std::string::npos) return 4;
		if (codec.find("vp9") != std::string::npos ||
			codec.find("vp09") != std::string::npos)
			return 3;
		if (codec.find("avc1") != std::string::npos ||
			codec.find("h264") != std::string::npos)
			return 2;
		if (codec.find("vp8") != std::string::npos) return 1;
		return 0;
	};

	auto get_acodec_score = [](const std::string &codec) -> int {
		if (codec.find("opus") != std::string::npos) return 4;
		if (codec.find("vorbis") != std::string::npos) return 3;
		if (codec.find("mp4a") != std::string::npos ||
			codec.find("aac") != std::string::npos)
			return 2;
		return 0;
	};

	// If explicit sort order is provided, use it
	if (!sort_order.empty()) {
		struct SortRule {
			std::string field;
			bool ascending;	 // true = prefers smaller/lower, false = prefers
							 // larger/higher
		};
		std::vector<SortRule> rules;

		std::string_view rem = sort_order;
		while (!rem.empty()) {
			size_t comma = rem.find(',');
			std::string_view token =
				(comma == std::string_view::npos) ? rem : rem.substr(0, comma);

			bool asc = false;
			if (!token.empty() && token[0] == '+') {
				asc = true;
				token.remove_prefix(1);
			}
			rules.push_back({std::string(token), asc});

			if (comma == std::string_view::npos) break;
			rem.remove_prefix(comma + 1);
		}

		auto compare_formats =
			[&](const VideoFormat &a, const VideoFormat &b) -> int {
			// Returns 1 if a > b (better), -1 if a < b (worse), 0 if equal
			for (const auto &rule : rules) {
				long long val_a = 0, val_b = 0;
				bool handled = false;
				double d_a = 0, d_b = 0;
				bool use_double = false;

				if (rule.field == "res") {
					val_a = (long long)a.width * a.height;
					val_b = (long long)b.width * b.height;
					handled = true;
				} else if (rule.field == "fps") {
					val_a = a.fps;
					val_b = b.fps;
					handled = true;
				} else if (rule.field == "size") {
					val_a = a.filesize_approx > 0 ? a.filesize_approx
												  : a.content_length;
					val_b = b.filesize_approx > 0 ? b.filesize_approx
												  : b.content_length;
					handled = true;
				} else if (rule.field == "br" || rule.field == "tbr") {
					d_a = a.tbr > 0 ? a.tbr : (a.vbr + a.abr);
					d_b = b.tbr > 0 ? b.tbr : (b.vbr + b.abr);
					use_double = true;
					handled = true;
				} else if (rule.field == "proto") {
					constexpr int SCORE_HTTPS = 20;
					constexpr int SCORE_M3U8_NATIVE = 5;
					constexpr int SCORE_M3U8 = 4;

					auto get_proto_score = [&](const std::string &p) {
						if (p == "https" || p == "http") return SCORE_HTTPS;
						if (p == "m3u8_native") return SCORE_M3U8_NATIVE;
						if (p == "m3u8") return SCORE_M3U8;
						return 0;
					};
					val_a = get_proto_score(a.protocol);
					val_b = get_proto_score(b.protocol);
					handled = true;
				} else if (rule.field == "ext") {
					constexpr int SCORE_MP4 = 10;
					constexpr int SCORE_M4A = 9;
					constexpr int SCORE_WEBM = 8;

					auto get_ext_score = [&](const std::string &e) {
						if (e == "mp4") return SCORE_MP4;
						if (e == "m4a") return SCORE_M4A;
						if (e == "webm") return SCORE_WEBM;
						return 0;
					};
					val_a = get_ext_score(a.ext);
					val_b = get_ext_score(b.ext);
					handled = true;
				} else if (rule.field == "codec" || rule.field == "vcodec" ||
						   rule.field == "acodec") {
					val_a =
						get_vcodec_score(a.vcodec) + get_acodec_score(a.acodec);
					val_b =
						get_vcodec_score(b.vcodec) + get_acodec_score(b.acodec);
					handled = true;
				}

				if (handled) {
					bool better = false;
					bool worse = false;
					if (use_double) {
						constexpr double BITRATE_EPSILON = 0.001;
						if (std::abs(d_a - d_b) > BITRATE_EPSILON) {
							if (rule.ascending)
								better = d_a < d_b;
							else
								better = d_a > d_b;

							if (rule.ascending)
								worse = d_a > d_b;
							else
								worse = d_a < d_b;
						}
					} else {
						if (val_a != val_b) {
							if (rule.ascending)
								better = val_a < val_b;
							else
								better = val_a > val_b;

							if (rule.ascending)
								worse = val_a > val_b;
							else
								worse = val_a < val_b;
						}
					}

					if (better) return 1;
					if (worse) return -1;
				}
			}
			return 0;  // Equal
		};

		std::vector<const VideoFormat *> candidates;
		for (const auto &f : info.formats) {
			if (passes_filters(f)) candidates.push_back(&f);
		}

		// Sort: Strict Weak Ordering required. compare_formats returns 1
		// (better), -1 (worse). std::sort requires a < b. We want BEST first.
		// So we want a > b (Desc). Using the comparator logic: if a is better
		// than b (1), a comes first.
		std::stable_sort(candidates.begin(), candidates.end(),
						 [&](const VideoFormat *a, const VideoFormat *b) {
							 return compare_formats(*a, *b) == 1;
						 });

		if (candidates.empty()) return {};

		StreamInfo res;

		// If we are looking for separation (video+audio), we might need to be
		// careful. yt-dlp -S sorts formats. If the user asked for -f
		// "bestvideo+bestaudio" AND -S "res", we should select best video by
		// res, and best audio by res.

		// Split into video/audio lists
		std::vector<const VideoFormat *> v_cands, a_cands;
		for (const auto *f : candidates) {
			if (f->vcodec != "none") v_cands.push_back(f);
			if (f->acodec != "none") a_cands.push_back(f);
		}

		if (is_audio_only) {
			res.audio = a_cands.empty() ? nullptr : a_cands[0];
			return res;
		}
		if (is_video_only) {
			res.video = v_cands.empty() ? nullptr : v_cands[0];
			return res;
		}

		// Standard "best" behavior with custom sort:
		// We need a video stream and an audio stream.
		res.video = v_cands.empty() ? nullptr : v_cands[0];
		res.audio = a_cands.empty() ? nullptr : a_cands[0];

		// If the Top1 video is also a combined format (and valid audio), use
		// it? yt-dlp logic: If we have separate video and audio, we merge them
		// unless a combined format is better. For now, assume split is
		// preferred unless -f specified otherwise. But wait, "best" is usually
		// split.

		return res;
	}

	auto check_better_video =
		[&](const VideoFormat *base, const VideoFormat &f) -> bool {
		if (!base) return true;

		// Direction helpers
		auto is_better_dbl = [&](double a, double b) {
			return is_worst ? a < b : a > b;
		};
		auto is_better_int = [&](long long a, long long b) {
			return is_worst ? a < b : a > b;
		};
		auto is_best_pref = [&](int a, int b) {
			return is_worst ? a < b : a > b;
		};

		// 1. Resolution
		long long res_f = (long long)f.width * f.height;
		long long res_base = (long long)base->width * base->height;
		if (res_f != res_base) return is_better_int(res_f, res_base);

		// 2. FPS
		if (f.fps != base->fps) return is_better_int(f.fps, base->fps);

		// 3. Codec (Score) - Prefer modern codecs even if bitrate is lower
		int score_f = get_vcodec_score(f.vcodec);
		int score_base = get_vcodec_score(base->vcodec);
		if (score_f != score_base) return is_best_pref(score_f, score_base);

		// 4. TBR (Bitrate)
		if (f.tbr != base->tbr) return is_better_dbl(f.tbr, base->tbr);

		// 5. Source Preference
		if (f.source_preference != base->source_preference)
			return is_best_pref(f.source_preference, base->source_preference);

		return false;
	};

	auto check_better_audio =
		[&](const VideoFormat *base, const VideoFormat &f) -> bool {
		if (!base) return true;

		auto is_better_dbl = [&](double a, double b) {
			return is_worst ? a < b : a > b;
		};
		auto is_better_int = [&](long long a, long long b) {
			return is_worst ? a < b : a > b;
		};
		auto is_best_pref = [&](int a, int b) {
			return is_worst ? a < b : a > b;
		};

		// 1. Language Preference (Always prefer requested language)
		bool f_lang_match = preferred_lang && f.language == *preferred_lang;
		bool base_lang_match =
			preferred_lang && base->language == *preferred_lang;
		if (f_lang_match != base_lang_match) return f_lang_match;

		// 2. Audio Channels
		if (f.audio_channels != base->audio_channels)
			return is_better_int(f.audio_channels, base->audio_channels);

		// 3. Codec
		int score_f = get_acodec_score(f.acodec);
		int score_base = get_acodec_score(base->acodec);
		if (score_f != score_base) return is_best_pref(score_f, score_base);

		// 4. TBR / ABR
		double tbr_f = f.tbr > 0 ? f.tbr : f.abr;
		double tbr_base = base->tbr > 0 ? base->tbr : base->abr;
		if (tbr_f != tbr_base) return is_better_dbl(tbr_f, tbr_base);

		// 5. Source Preference
		if (f.source_preference != base->source_preference)
			return is_best_pref(f.source_preference, base->source_preference);

		return false;
	};

	const VideoFormat *best_v = nullptr;
	const VideoFormat *best_a = nullptr;
	const VideoFormat *best_combined = nullptr;

	for (const auto &f : info.formats) {
		// Filter Check
		if (!passes_filters(f)) continue;

		bool has_video = f.vcodec != "none";
		bool has_audio = f.acodec != "none";

		// Update Best Combined
		if (has_video && has_audio) {
			if (check_better_video(best_combined, f)) { best_combined = &f; }
		}

		// Update Best/Worst Video
		if (has_video) {
			if (check_better_video(best_v, f)) { best_v = &f; }
		}

		// Update Best/Worst Audio
		if (has_audio) {
			if (check_better_audio(best_a, f)) { best_a = &f; }
		}
	}

	// 1. Audio Only Selector
	if (is_audio_only) {
		result.audio = best_a;
		return result;
	}

	// 2. Video Only Selector
	if (is_video_only) {
		result.video = best_v;
		return result;
	}

	// 3. Generic "best" or "worst" (Combined Preference)
	// If we found a combined format that matches our criteria (best or worst)
	// We check if it satisfies 'best-ness' against the raw streams or if we
	// should merge.

	// For "worst", typically valid combined formats (18, 17) are lower quality
	// than adaptive streams (137+140).
	// So worst combined might actually be the global worst.
	if (is_worst) {
		// If we have a combined format, it's likely worse (res-wise) than high
		// quality adaptive.
		if (best_combined) {
			result.video = best_combined;
			result.audio = best_combined;
			return result;
		}
		// Fallback to split
		result.video = best_v;
		result.audio = best_a;
		return result;
	}

	// For "best" (Default)
	// Use combined if it matches the best video found (e.g. to avoid
	// unnecessary demux/mux if source is already good enough or strictly
	// better).
	if (best_combined && best_v && best_combined == best_v) {
		result.video = best_combined;
		result.audio = best_combined;
		return result;
	}

	// Otherwise, split streams
	result.video = best_v;
	result.audio = best_a;
	return result;
}

bool Downloader::Impl::merge_streams(const std::string &video_path,
									 const std::string &audio_path,
									 const std::string &output_path) {
	spdlog::info("Muxing video and audio to: {}", output_path);
	return media::Muxer::merge(video_path, audio_path, output_path);
}

// Downloader main methods delegation
Downloader::Downloader(std::shared_ptr<net::HttpClient> http)
	: m_impl(std::make_unique<Impl>(std::move(http))) {}

Downloader::~Downloader() = default;
Downloader::Downloader(Downloader &&) noexcept = default;
Downloader &Downloader::operator=(Downloader &&) noexcept = default;

asio::any_io_executor Downloader::get_executor() const {
	return m_impl->http->get_executor();
}

void Downloader::async_download_impl(
	const VideoInfo &info, DownloadOptions options,
	ProgressCallback progress_cb,
	asio::any_completion_handler<void(Result<std::string>)> handler,
	CompletionExecutor handler_ex) {
	m_impl->async_download(info, std::move(options), std::move(progress_cb),
						   std::move(handler), std::move(handler_ex));
}

Downloader::StreamInfo Downloader::select_streams(
	const VideoInfo &info, std::string_view selector,
	std::optional<std::string> preferred_lang, std::string_view sort_order) {
	return Impl::select_streams(
		info, selector, std::move(preferred_lang), sort_order);
}

}  // namespace ytdlpp
