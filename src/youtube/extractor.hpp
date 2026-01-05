#pragma once

#include <optional>
#include <string>
#include <vector>

#include "innertube.hpp"
#include "net/http_client.hpp"
#include "scripting/quickjs_engine.hpp"

namespace ytdlpp::youtube {

struct VideoFormat {
	int itag;
	std::string url;
	std::string mime_type;
	std::string ext;  // Deduced or verified
	std::string vcodec;
	std::string acodec;
	int width;
	int height;
	int fps;
	int audio_sample_rate;
	int audio_channels;
	double tbr;	 // Total bitrate
	double abr;	 // Audio bitrate
	double vbr;	 // Video bitrate
	long long content_length;
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

class Extractor {
   public:
	explicit Extractor(net::HttpClient &http, scripting::JsEngine &js);

	// Main Entry Point
	std::optional<VideoInfo> process(const std::string &url);

   private:
	net::HttpClient &http_;
	scripting::JsEngine &js_;

	// Tries to get info using a specific client context
	std::optional<nlohmann::json> get_info_with_client(
		const std::string &video_id, const InnertubeContext &client);

	// Descrambling logic
	void descramble_formats(std::vector<VideoFormat> &formats,
							const std::string &player_url);
	std::string extract_video_id(const std::string &url);
};

// JSON Serialization
void to_json(nlohmann::json &j, const VideoFormat &f);
void to_json(nlohmann::json &j, const VideoInfo &i);

}  // namespace ytdlpp::youtube
