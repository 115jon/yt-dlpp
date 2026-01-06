#pragma once

#include <cstdint>	// Added for uint8_t
#include <functional>
#include <string>
#include <vector>

namespace ytdlpp {

struct DownloadProgress {
	long long total_downloaded_bytes;
	long long total_size_bytes;
	double percentage;
	double speed_bytes_per_sec;
	double eta_seconds;
};

using ProgressCallback = std::function<void(
	const std::string &status, const DownloadProgress &progress)>;
using StreamDataCallback = std::function<void(const std::vector<uint8_t> &)>;

struct VideoFormat {
	int itag = 0;
	std::string url;
	std::string mime_type;
	std::string ext;
	std::string vcodec;
	std::string acodec;
	int width = 0;
	int height = 0;
	int fps = 0;
	int audio_sample_rate = 0;
	int audio_channels = 0;
	double tbr = 0.0;
	double abr = 0.0;
	double vbr = 0.0;
	long long content_length = 0;
	std::string language;		   // Language code
	int language_preference = -1;  // Default -1

	// Additional yt-dlp compatible fields
	std::string format_note;	// e.g., "144p", "720p", "Premium"
	std::string container;		// e.g., "webm", "mp4"
	std::string protocol;		// e.g., "https", "m3u8_native"
	std::string dynamic_range;	// e.g., "SDR", "HDR"
	double aspect_ratio = 0.0;
	bool has_drm = false;
	long long filesize_approx = 0;
};

// Represents a chapter in a video
struct Chapter {
	double start_time = 0.0;
	double end_time = 0.0;
	std::string title;
};

// Represents a thumbnail
struct Thumbnail {
	std::string url;
	int width = 0;
	int height = 0;
	std::string id;
};

struct VideoInfo {
	std::string id;
	std::string title;
	std::string fulltitle;	// Full title with special chars
	std::string description;
	std::string uploader;
	std::string uploader_id;
	std::string uploader_url;
	std::string upload_date;  // YYYYMMDD format
	long long duration = 0;
	std::string duration_string;  // e.g., "3:33"
	long long view_count = 0;
	long long like_count = 0;
	long long comment_count = 0;
	std::string webpage_url;
	std::string thumbnail;
	std::vector<Thumbnail> thumbnails;
	std::vector<VideoFormat> formats;

	// Additional yt-dlp compatible fields
	std::string channel;
	std::string channel_id;
	std::string channel_url;
	long long channel_follower_count = 0;
	bool channel_is_verified = false;

	std::vector<std::string> categories;
	std::vector<std::string> tags;
	std::vector<Chapter> chapters;

	int age_limit = 0;
	std::string availability;  // "public", "unlisted", "private", "needs_auth"
	std::string live_status;   // "not_live", "is_live", "was_live", "post_live"
	bool playable_in_embed = true;
	bool is_live = false;
	bool was_live = false;

	long long timestamp = 0;   // Unix timestamp of upload
	std::string release_date;  // Release date YYYYMMDD

	// Extracted format info (set after format selection)
	std::string ext;		 // Final extension
	std::string format;		 // Format description
	std::string format_id;	 // Selected format ID
	std::string resolution;	 // e.g., "1920x1080"

	// Type info
	std::string extractor = "youtube";
	std::string extractor_key = "Youtube";
	std::string _type = "video";
};

}  // namespace ytdlpp
