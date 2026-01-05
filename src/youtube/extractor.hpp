#pragma once

#include <optional>
#include <string>
#include <vector>
#include <ytdlpp/result.hpp>
#include <ytdlpp/types.hpp>

#include "innertube.hpp"
#include "net/http_client.hpp"
#include "scripting/quickjs_engine.hpp"

namespace ytdlpp::youtube {

using VideoFormat = ytdlpp::VideoFormat;
using VideoInfo = ytdlpp::VideoInfo;

class Extractor {
   public:
	explicit Extractor(net::HttpClient &http, scripting::JsEngine &js);

	// Main Entry Point
	Result<VideoInfo> process(const std::string &url);

   private:
	net::HttpClient &http_;
	scripting::JsEngine &js_;

	// Tries to get info using a specific client context
	Result<nlohmann::json> get_info_with_client(const std::string &video_id,
												const InnertubeContext &client);

	std::string extract_video_id(const std::string &url);
};

// JSON Serialization
void to_json(nlohmann::json &j, const VideoFormat &f);
void to_json(nlohmann::json &j, const VideoInfo &i);

}  // namespace ytdlpp::youtube
