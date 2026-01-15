#include <spdlog/spdlog.h>

#include <algorithm>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <filesystem>
#include <utility>
#include <ytdlpp/downloader.hpp>
#include <ytdlpp/http_client.hpp>

namespace fs = std::filesystem;

#include "media/muxer.hpp"

namespace ytdlpp {

namespace {
std::string sanitize_filename_local(const std::string &name) {
	std::string out = name;
	const std::string invalid = "\\/:*?\"<>|";
	for (char &c : out) {
		if (invalid.find(c) != std::string::npos) c = '_';
	}
	return out;
}
}  // namespace

// Struct Impl definition
struct Downloader::Impl {
	std::shared_ptr<ytdlpp::net::HttpClient> http;

	explicit Impl(std::shared_ptr<ytdlpp::net::HttpClient> h)
		: http(std::move(h)) {}

	// Logic for stream selection
	static Downloader::StreamInfo select_streams(
		const VideoInfo &info, std::string_view selector,
		std::optional<std::string> preferred_lang = std::nullopt);

	// Async download logic delegate
	void async_download(
		const ytdlpp::VideoInfo &info, std::string format_selector,
		std::optional<std::string> merge_format,
		ytdlpp::ProgressCallback progress_cb,
		asio::any_completion_handler<void(Result<std::string>)> handler,
		Downloader::CompletionExecutor handler_ex);

   private:
	// Helpers
	static bool merge_streams(const std::string &video_path,
							  const std::string &audio_path,
							  const std::string &output_path);
	static std::string sanitize_filename(const std::string &name);
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

	void start(const VideoInfo &info, std::string_view selector,
			   std::optional<std::string> merge_fmt) {
		info_ = info;
		merge_fmt_ = std::move(merge_fmt);

		streams_ = Downloader::select_streams(info_, selector);

		if (!streams_.video && !streams_.audio) {
			return complete(outcome::failure(errc::video_not_found));
		}

		base_filename_ = sanitize_filename_local(info.title);
		if (base_filename_.empty()) base_filename_ = "video";

		// Calculate total operations
		active_downloads_ = 0;
		if (streams_.video) active_downloads_++;
		if (streams_.audio) active_downloads_++;

		if (streams_.video) download_video();
		if (streams_.audio) download_audio();
	}

   private:
	std::shared_ptr<ytdlpp::net::HttpClient> http_;
	asio::any_completion_handler<void(Result<std::string>)> cb_;
	CompletionExecutor handler_ex_;
	ProgressCallback progress_cb_;
	VideoInfo info_;
	std::optional<std::string> merge_fmt_;
	Downloader::StreamInfo streams_;
	std::string base_filename_;
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
		video_path_ = base_filename_ + "." + streams_.video->ext;

		spdlog::info("Downloading video: {}", video_path_);
		http_->async_download_file(
			streams_.video->url, video_path_,
			[this](long long now, long long total) {
				current_video_bytes_ = now;
				if (total > 0) total_video_bytes_ = total;
				report_progress("downloading video");
			},
			[self = shared_from_this()](Result<void> res) mutable {
				if (res.has_error()) {
					spdlog::error(
						"Video download failed: {}", res.error().message());
					self->error_occurred_ = true;
				}
				self->on_download_complete();
			});
	}

	void download_audio() {
		audio_path_ = base_filename_ + "_audio." + streams_.audio->ext;
		spdlog::info("Downloading audio: {}", audio_path_);
		http_->async_download_file(
			streams_.audio->url, audio_path_,
			[this](long long now, long long total) {
				current_audio_bytes_ = now;
				if (total > 0) total_audio_bytes_ = total;
				report_progress("downloading audio");
			},
			[self = shared_from_this()](Result<void> res) mutable {
				if (res.has_error()) {
					spdlog::error(
						"Audio download failed: {}", res.error().message());
					self->error_occurred_ = true;
				}
				self->on_download_complete();
			});
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
			prog.percentage = (double)total_current / total_size * 100.0;
		}

		if (start_time_.time_since_epoch().count() > 0) {
			auto now = std::chrono::steady_clock::now();
			auto duration =
				std::chrono::duration_cast<std::chrono::milliseconds>(
					now - start_time_)
					.count();

			if (duration > 0) {
				prog.speed_bytes_per_sec =
					(double)total_current * 1000.0 / duration;

				if (prog.speed_bytes_per_sec > 0 && total_size > 0) {
					long long remaining = total_size - total_current;
					prog.eta_seconds =
						(double)remaining / prog.speed_bytes_per_sec;
				}
			}
		}

		progress_cb_(status, prog);
	}

	void on_download_complete() {
		if (--active_downloads_ == 0) {
			if (error_occurred_) {
				return complete(outcome::failure(errc::request_failed));
			}
			finalize();
		}
	}

	void finalize() {
		if (streams_.video && streams_.audio && merge_fmt_) {
			spdlog::info("Merging video and audio...");
			report_progress("merging");

			std::string output_filename = base_filename_ + "." + *merge_fmt_;

			if (media::Muxer::merge(
					video_path_, audio_path_, output_filename)) {
				std::filesystem::remove(video_path_);
				std::filesystem::remove(audio_path_);
				return complete(outcome::success(output_filename));
			}
			spdlog::error("Merge failed");
			return complete(outcome::failure(errc::muxer_error));
		}
		// If not merging, just return video path for now or maybe both?
		// The user interface assumes single file return usually.
		// Let's return video path.
		return complete(outcome::success(video_path_));
	}

	void complete(Result<std::string> res) {
		asio::dispatch(
			handler_ex_, [cb = std::move(cb_), res = std::move(res)]() mutable {
				cb(std::move(res));
			});
	}
};

void Downloader::Impl::async_download(
	const ytdlpp::VideoInfo &info, std::string format_selector,
	std::optional<std::string> merge_format,
	ytdlpp::ProgressCallback progress_cb,
	asio::any_completion_handler<void(Result<std::string>)> handler,
	Downloader::CompletionExecutor handler_ex) {
	std::make_shared<AsyncDownloaderSession>(
		http, std::move(handler), std::move(handler_ex), std::move(progress_cb))
		->start(info, format_selector, std::move(merge_format));
}

Downloader::StreamInfo Downloader::Impl::select_streams(
	const VideoInfo &info, std::string_view selector,
	std::optional<std::string> preferred_lang) {
	StreamInfo result;

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

	spdlog::debug(
		"Sort order given by extractor: quality, res, fps, hdr:12, source, "
		"vcodec, channels, acodec, lang, proto");

	if (selector == "bestaudio") {
		const VideoFormat *best = nullptr;
		const VideoFormat *preferred =
			nullptr;  // Track preferred language match
		for (const auto &f : info.formats) {
			if (f.vcodec == "none" && f.acodec != "none") {
				// Check if format matches preferred language
				bool is_preferred =
					preferred_lang && f.language == *preferred_lang;

				auto compare_audio = [&](const VideoFormat *current,
										 const VideoFormat &candidate) {
					if (!current) return true;
					if (candidate.language_preference !=
						current->language_preference)
						return candidate.language_preference >
							   current->language_preference;
					if (candidate.audio_channels != current->audio_channels)
						return candidate.audio_channels >
							   current->audio_channels;
					int score_new = get_acodec_score(candidate.acodec);
					int score_best = get_acodec_score(current->acodec);
					if (score_new != score_best) return score_new > score_best;
					return candidate.tbr > current->tbr;
				};

				if (is_preferred) {
					if (compare_audio(preferred, f)) preferred = &f;
				}
				if (compare_audio(best, f)) best = &f;
			}
		}
		// Prefer exact language match, fallback to best overall
		const VideoFormat *selected = preferred ? preferred : best;
		if (selected)
			spdlog::info(
				"Selected best audio: itag={}, ext={}, tbr={:.2f}, acodec={}, "
				"channels={}, lang={}, lang_pref={}",
				selected->itag, selected->ext, selected->tbr, selected->acodec,
				selected->audio_channels, selected->language,
				selected->language_preference);
		result.audio = selected;
		return result;
	}

	const VideoFormat *best_video = nullptr;
	const VideoFormat *best_audio = nullptr;
	const VideoFormat *best_audio_preferred_ = nullptr;

	spdlog::debug("Sorting video formats by: res, fps, vcodec, tbr");

	for (const auto &f : info.formats) {
		if (f.vcodec != "none") {
			if (!best_video)
				best_video = &f;
			else {
				bool is_better = false;
				long long res_new = (long long)f.width * f.height;
				long long res_best =
					(long long)best_video->width * best_video->height;
				if (res_new != res_best) {
					is_better = res_new > res_best;
				} else if (f.fps != best_video->fps) {
					is_better = f.fps > best_video->fps;
				} else {
					int c_new = get_vcodec_score(f.vcodec);
					int c_best = get_vcodec_score(best_video->vcodec);
					if (c_new != c_best) {
						is_better = c_new > c_best;
					} else {
						is_better = f.tbr > best_video->tbr;
					}
				}
				if (is_better) best_video = &f;
			}
		}

		if (f.acodec != "none" && f.vcodec == "none") {
			bool is_preferred = preferred_lang && f.language == *preferred_lang;

			auto compare_audio = [&](const VideoFormat *current,
									 const VideoFormat &candidate) {
				if (!current) return true;
				if (candidate.language_preference !=
					current->language_preference)
					return candidate.language_preference >
						   current->language_preference;
				if (candidate.audio_channels != current->audio_channels)
					return candidate.audio_channels > current->audio_channels;
				int s_new = get_acodec_score(candidate.acodec);
				int s_best = get_acodec_score(current->acodec);
				if (s_new != s_best) return s_new > s_best;
				return candidate.tbr > current->tbr;
			};

			if (is_preferred) {
				if (!best_audio_preferred_ ||
					compare_audio(best_audio_preferred_, f))
					best_audio_preferred_ = &f;
			}
			if (compare_audio(best_audio, f)) best_audio = &f;
		}
	}

	result.video = best_video;
	// Prefer exact language match, fallback to best overall
	result.audio = best_audio_preferred_ ? best_audio_preferred_ : best_audio;
	return result;
}

bool Downloader::Impl::merge_streams(const std::string &video_path,
									 const std::string &audio_path,
									 const std::string &output_path) {
	spdlog::info("Muxing video and audio to: {}", output_path);
	return media::Muxer::merge(video_path, audio_path, output_path);
}

std::string Downloader::Impl::sanitize_filename(const std::string &name) {
	std::string result = name;
	const std::string illegal = "<>:\"/\\|?*";
	for (char c : illegal) {
		std::replace(result.begin(), result.end(), c, '_');
	}
	result.erase(std::remove_if(result.begin(), result.end(),
								[](unsigned char c) { return c < 32; }),
				 result.end());
	return result;
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
	const VideoInfo &info, std::string format_selector,
	std::optional<std::string> merge_format, ProgressCallback progress_cb,
	asio::any_completion_handler<void(Result<std::string>)> handler,
	CompletionExecutor handler_ex) {
	m_impl->async_download(
		info, std::move(format_selector), std::move(merge_format),
		std::move(progress_cb), std::move(handler), std::move(handler_ex));
}

Downloader::StreamInfo Downloader::select_streams(
	const VideoInfo &info, std::string_view selector,
	std::optional<std::string> preferred_lang) {
	return Impl::select_streams(info, selector, std::move(preferred_lang));
}

}  // namespace ytdlpp
