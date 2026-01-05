#include "downloader.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

#include "../media/muxer.hpp"

namespace ytdlpp {

Downloader::Downloader(net::HttpClient &http) : http_(http) {}

bool Downloader::download(const youtube::VideoInfo &info,
						  const std::string &format_selector,
						  const std::optional<std::string> &merge_format) {
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
		video_path =
			base_filename + ".f" + std::to_string(streams.video->itag) + "." +
			streams.video->ext;
		spdlog::info("Downloading video stream: {}", video_path);
		if (!fs::exists(video_path)) {
			video_success = download_stream(*streams.video, video_path);
		}
	}

	if (streams.audio && (!streams.video || streams.audio != streams.video)) {
		audio_path =
			base_filename + ".f" + std::to_string(streams.audio->itag) + "." +
			streams.audio->ext;
		spdlog::info("Downloading audio stream: {}", audio_path);
		if (!fs::exists(audio_path)) {
			audio_success = download_stream(*streams.audio, audio_path);
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
		} else {
			spdlog::error("Merge failed. Keeping separate files.");
			return false;
		}
	} else {
		// Just rename single file if needed
		std::string src = streams.video ? video_path : audio_path;
		if (src != final_path) { fs::rename(src, final_path); }
		spdlog::info("Downloaded: {}", final_path);
		return true;
	}
}

Downloader::StreamInfo Downloader::select_streams(
	const youtube::VideoInfo &info, const std::string &selector) {
	StreamInfo result;

	// Simplified selector logic
	// "best" -> bestvideo+bestaudio
	// "bestaudio" -> best audio only
	// "bestvideo" -> best video only
	// For now we assume "best" means finding highest res video and highest
	// bitrate audio

	if (selector == "bestaudio") {
		const youtube::VideoFormat *best = nullptr;
		double best_score = -1.0;

		auto get_audio_score = [](const youtube::VideoFormat &f) -> double {
			double score = f.tbr;
			// Prefer Opus (WebM) over AAC (M4A) slightly, as it's a
			// newer/better codec Adding a small bias (e.g. 10 kbps equivalent)
			// allows picking Opus 122k over AAC 129k
			if (f.acodec.find("opus") != std::string::npos) { score += 10.0; }
			return score;
		};

		for (const auto &f : info.formats) {
			if (f.vcodec == "none" && f.acodec != "none") {
				double score = get_audio_score(f);
				if (!best || score > best_score) {
					best = &f;
					best_score = score;
				}
			}
		}
		if (best)
			spdlog::info("Selected best audio: itag={}, ext={}, tbr={:.2f}",
						 best->itag, best->ext, best->tbr);
		result.audio = best;
		return result;
	}

	// "best" or default
	const youtube::VideoFormat *best_video = nullptr;
	const youtube::VideoFormat *best_audio = nullptr;
	const youtube::VideoFormat *best_audio_only = nullptr;

	for (const auto &f : info.formats) {
		if (f.vcodec != "none") {
			// Prefer higher res, then tbr
			if (!best_video)
				best_video = &f;
			else {
				if (f.height > best_video->height)
					best_video = &f;
				else if (f.height == best_video->height &&
						 f.tbr > best_video->tbr)
					best_video = &f;
			}
		}

		if (f.acodec != "none") {
			// Track best overall audio (including mixed)
			if (!best_audio || f.tbr > best_audio->tbr) best_audio = &f;

			// Track best pure audio (vcodec == none)
			if (f.vcodec == "none") {
				// Determine score for pure audio (prefer opus/higher tbr)
				// Similar logic to bestaudio selector
				auto score = [](const youtube::VideoFormat &fmt) {
					double s = fmt.tbr;
					if (fmt.acodec.find("opus") != std::string::npos) s += 10.0;
					return s;
				};

				if (!best_audio_only || score(f) > score(*best_audio_only)) {
					best_audio_only = &f;
				}
			}
		}
	}

	result.video = best_video;
	// Prefer pure audio stream if available, otherwise fallback to best mixed
	result.audio = best_audio_only ? best_audio_only : best_audio;

	// If video stream also extracts audio (progressive), we check if separate
	// audio is better But typically 1080p is video-only. If result.video has
	// acodec != none, we might not need separate audio unless it's low quality

	if (result.video && result.video->acodec != "none") {
		// If we found a standalone high quality audio, use it for merging
		// Otherwise, if the video file already has sound, we might just use it
		// unless format selector specifically asked for "best" which implies
		// best quality. Currently we stick to: if we found a better separate
		// audio, use it.
	}

	return result;
}

bool Downloader::download_stream(const youtube::VideoFormat &format,
								 const std::string &output_path) {
	auto start_time = std::chrono::steady_clock::now();
	auto last_log = start_time;

	bool result = http_.download_file(
		format.url, output_path, [&](long long now, long long total) {
			auto current_time = std::chrono::steady_clock::now();
			// Log every 200ms
			if (std::chrono::duration_cast<std::chrono::milliseconds>(
					current_time - last_log)
						.count() > 200 ||
				now == total) {
				last_log = current_time;

				double speed = 0.0;
				auto duration =
					std::chrono::duration_cast<std::chrono::seconds>(
						current_time - start_time)
						.count();
				if (duration > 0)
					speed = (double)now / duration / 1024.0 / 1024.0;

				if (total > 0) {
					int percent = (int)(now * 100 / total);
					std::cout << "\rProgress: " << percent << "% ("
							  << (now / 1024 / 1024) << " MB / "
							  << (total / 1024 / 1024) << " MB) @ "
							  << fmt::format("{:.2f}", speed) << " MB/s   "
							  << std::flush;
				} else {
					std::cout << "\rProgress: " << (now / 1024 / 1024)
							  << " MB @ " << fmt::format("{:.2f}", speed)
							  << " MB/s   " << std::flush;
				}
			}
		});
	std::cout << "\n";
	return result;
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
