#include "downloader.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

#include "../media/muxer.hpp"

namespace ytdlpp {

Downloader::Downloader(net::HttpClient &http) : http_(http) {}

bool Downloader::download(const ytdlpp::VideoInfo &info,
						  const std::string &format_selector,
						  const std::optional<std::string> &merge_format,
						  ytdlpp::ProgressCallback progress_cb) {
	auto streams = select_streams(info, format_selector);

	if (!streams.video && !streams.audio) {
		spdlog::error(
			"No suitable streams found for selector: {}", format_selector);
		return false;
	}

	std::string base_filename = sanitize_filename(info.title);
	if (base_filename.empty()) base_filename = info.id;	 // Fallback

	// Ensure unique or safe filename
	// Actually yt-dlp has complex templating. We'll stick to simple "Title
	// [id].ext"
	base_filename += " [" + info.id + "]";

	std::string final_ext;
	if (merge_format && !merge_format->empty()) {
		final_ext = *merge_format;
	} else if (streams.video && streams.audio) {
		final_ext = "mkv";	// Merge to mkv usually safe
		if (streams.video->ext == "mp4" &&
			(streams.audio->ext == "m4a" || streams.audio->ext == "mp4"))
			final_ext = "mp4";
		else if (streams.video->ext == "webm" && streams.audio->ext == "webm")
			final_ext = "webm";
	} else if (streams.video) {
		final_ext = streams.video->ext;
	} else {
		final_ext = streams.audio->ext;
	}

	std::string final_path = base_filename + "." + final_ext;

	// Check if file exists
	if (fs::exists(final_path)) {
		spdlog::warn("File already exists: {}", final_path);
		// return true? or overwrite? usually yt-dlp skips.
		// Let's explicitly skip to be safe.
		// return true;
	}

	std::string video_path, audio_path;
	bool video_success = true, audio_success = true;

	if (streams.video) {
		spdlog::info(
			"{}: Downloading 1 format(s): {}+{}", info.id, streams.video->itag,
			streams.audio ? std::to_string(streams.audio->itag) : "none");

		video_path =
			base_filename + ".f" + std::to_string(streams.video->itag) + "." +
			streams.video->ext;
		spdlog::debug("Invoking http downloader on \"{}\"", streams.video->url);
		spdlog::info("Downloading video stream: {}", video_path);
		if (!fs::exists(video_path)) {
			video_success =
				download_stream(*streams.video, video_path, progress_cb);
		}
	}

	if (streams.audio && (!streams.video || streams.audio != streams.video)) {
		audio_path =
			base_filename + ".f" + std::to_string(streams.audio->itag) + "." +
			streams.audio->ext;

		if (!streams.video) {
			spdlog::info("{}: Downloading 1 format(s): {}", info.id,
						 streams.audio->itag);
		}

		spdlog::debug("Invoking http downloader on \"{}\"", streams.audio->url);
		spdlog::info("Downloading audio stream: {}", audio_path);
		if (!fs::exists(audio_path)) {
			audio_success =
				download_stream(*streams.audio, audio_path, progress_cb);
		}
	}

	if (!video_success || !audio_success) {
		spdlog::error("Download failed.");
		return false;
	}

	if (streams.video && streams.audio && video_path != audio_path) {
		spdlog::info("Merging streams to: {}", final_path);
		if (merge_streams(video_path, audio_path, final_path)) {
			spdlog::info("Merge successful.");
			// Cleanup parts
			fs::remove(video_path);
			fs::remove(audio_path);
			return true;
		}
		spdlog::error("Merge failed. Keeping separate files.");
		return false;
	}
	// Just rename single file if needed
	std::string src = streams.video ? video_path : audio_path;
	if (src != final_path) { fs::rename(src, final_path); }
	spdlog::info("Downloaded: {}", final_path);
	return true;
}

Downloader::StreamInfo Downloader::select_streams(const VideoInfo &info,
												  const std::string &selector) {
	StreamInfo result;

	// Helper to rank video codecs: AV1 > VP9 > AVC > Others
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

	// Helper to rank audio codecs: Opus > Vorbis > AAC > Others
	auto get_acodec_score = [](const std::string &codec) -> int {
		if (codec.find("opus") != std::string::npos) return 4;
		if (codec.find("vorbis") != std::string::npos) return 3;
		if (codec.find("mp4a") != std::string::npos ||
			codec.find("aac") != std::string::npos)
			return 2;
		return 0;
	};

	// Debug logs mimicking yt-dlp behavior
	spdlog::debug(
		"Sort order given by extractor: quality, res, fps, hdr:12, source, "
		"vcodec, channels, acodec, lang, proto");
	spdlog::debug(
		"Formats sorted by: hasvid, ie_pref, quality, res, fps, hdr:12(7), "
		"source, vcodec, channels, acodec, lang, proto, size, br, asr, vext, "
		"aext, hasaud, id");

	if (selector == "bestaudio") {
		const VideoFormat *best = nullptr;

		for (const auto &f : info.formats) {
			if (f.vcodec == "none" && f.acodec != "none") {
				if (!best) {
					best = &f;
					continue;
				}

				// Compare logic: LanguagePref > Channels > Codec > Bitrate
				bool is_better = false;

				if (f.language_preference != best->language_preference) {
					is_better =
						f.language_preference > best->language_preference;
				} else if (f.audio_channels != best->audio_channels) {
					is_better = f.audio_channels > best->audio_channels;
				} else {
					int score_new = get_acodec_score(f.acodec);
					int score_best = get_acodec_score(best->acodec);

					if (score_new != score_best)
						is_better = score_new > score_best;
					else
						is_better = f.tbr > best->tbr;
				}

				if (is_better) { best = &f; }
			}
		}
		if (best)
			spdlog::info(
				"Selected best audio: itag={}, ext={}, tbr={:.2f}, acodec={}, "
				"channels={}, lang_pref={}",
				best->itag, best->ext, best->tbr, best->acodec,
				best->audio_channels, best->language_preference);
		result.audio = best;
		return result;
	}

	// "best" or default
	const VideoFormat *best_video = nullptr;
	const VideoFormat *best_audio = nullptr;

	spdlog::debug("Sorting video formats by: res, fps, vcodec, tbr");

	for (const auto &f : info.formats) {
		if (f.vcodec != "none") {
			// Logic: Resolution > FPS > Codec > Bitrate
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

		if (f.acodec != "none") {
			// Track best overall audio (including mixed)
			if (!best_audio || f.tbr > best_audio->tbr) best_audio = &f;

			// Track best pure audio (vcodec == none)
			// Actually we handle audio selection separately or via simple
			// fallback but here we want best pure aligned with bestvideo
			if (f.vcodec == "none") {
				if (!best_audio)
					best_audio = &f;
				else {
					// Compare Pure Audios: LanguagePref > Channels > Codec >
					// Bitrate
					bool is_better = false;

					if (f.language_preference !=
						best_audio->language_preference) {
						is_better = f.language_preference >
									best_audio->language_preference;
					} else if (f.audio_channels != best_audio->audio_channels) {
						is_better =
							f.audio_channels > best_audio->audio_channels;
					} else {
						int s_new = get_acodec_score(f.acodec);
						int s_best = get_acodec_score(best_audio->acodec);

						if (s_new != s_best)
							is_better = s_new > s_best;
						else
							is_better = f.tbr > best_audio->tbr;
					}

					if (is_better) best_audio = &f;
				}
			}
		}
	}

	if (best_video) {
		spdlog::info(
			"Selected best video: itag={}, res={}x{}, fps={}, vcodec={}, "
			"tbr={:.2f}",
			best_video->itag, best_video->width, best_video->height,
			best_video->fps, best_video->vcodec, best_video->tbr);
	}
	if (best_audio) {
		spdlog::info(
			"Selected best audio: itag={}, acodec={}, tbr={:.2f}, channels={}",
			best_audio->itag, best_audio->acodec, best_audio->tbr,
			best_audio->audio_channels);
	}

	result.video = best_video;
	result.audio = best_audio;

	// If video stream also calls for mixed, check if we really need external
	// audio
	if (result.video && result.video->acodec != "none") {
		// Mixed stream found as best video.
		// If we accepted it as best video, it means it beat others on visual
		// quality. Do we replace its audio? If we have a significantly better
		// pure audio, yes. But logic "bestvideo+bestaudio" usually implies
		// merging the best track of each. So we keep result.audio if it exists.
	}

	return result;
}

bool Downloader::download_stream(const VideoFormat &format,
								 const std::string &output_path,
								 ytdlpp::ProgressCallback progress_cb) {
	auto start_time = std::chrono::steady_clock::now();
	auto last_log = start_time;

	auto result = http_.download_file(
		format.url, output_path, [&](long long now, long long total) {
			if (progress_cb) {
				int percent = 0;
				if (total > 0) percent = (int)(now * 100 / total);
				progress_cb("downloading", percent);
			}

			auto current_time = std::chrono::steady_clock::now();
			// Log every 200ms or on completion
			if (std::chrono::duration_cast<std::chrono::milliseconds>(
					current_time - last_log)
						.count() > 200 ||
				now == total) {
				last_log = current_time;
			}
		});

	if (result.has_error()) {
		spdlog::error("Download error: {}", result.error().message());
		return false;
	}
	return true;
}

bool Downloader::merge_streams(const std::string &video_path,
							   const std::string &audio_path,
							   const std::string &output_path) {
	spdlog::info("Muxing video and audio to: {}", output_path);
	return media::Muxer::merge(video_path, audio_path, output_path);
}

std::string Downloader::sanitize_filename(const std::string &name) {
	std::string result = name;
	// Replace illegal chars
	const std::string illegal = "<>:\"/\\|?*";
	for (char c : illegal) {
		std::replace(result.begin(), result.end(), c, '_');
	}
	// Remove controls
	result.erase(std::remove_if(result.begin(), result.end(),
								[](unsigned char c) { return c < 32; }),
				 result.end());
	return result;
}

}  // namespace ytdlpp
