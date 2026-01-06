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
	int itag;
	std::string url;
	std::string mime_type;
	std::string ext;
	std::string vcodec;
	std::string acodec;
	int width;
	int height;
	int fps;
	int audio_sample_rate;
	int audio_channels;
	double tbr;
	double abr;
	double vbr;
	long long content_length;
	std::string language;		   // Language code
	int language_preference = -1;  // Default -1
};

struct VideoInfo {
	std::string id;
	std::string title;
	std::string description;
	std::string uploader;
	std::string uploader_id;
	std::string upload_date;
	long long duration;
	long long view_count;
	long long like_count;
	std::string webpage_url;
	std::string thumbnail;
	std::vector<VideoFormat> formats;
};

}  // namespace ytdlpp
