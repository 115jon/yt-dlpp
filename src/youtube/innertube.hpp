#pragma once

#include <map>
#include <nlohmann/json.hpp>
#include <string>

namespace ytdlpp::youtube {

struct InnertubeContext {
	std::string client_name;
	std::string client_version;
	std::string user_agent;
	std::string os_name;
	std::string os_version;
	std::string platform;	  // "MOBILE", "DESKTOP", "TV"
	std::string device_make;  // "Google", "Apple"
	std::string device_model;
	int client_id = 1;	// X-YouTube-Client-Name value
						// Client IDs from yt-dlp:
						// 1 = WEB
						// 2 = MWEB
						// 3 = ANDROID
						// 5 = IOS
						// 7 = TVHTML5
};

class Innertube {
   public:
	// Standard clients - require PO Token or JS player
	static const InnertubeContext CLIENT_ANDROID;
	static const InnertubeContext CLIENT_IOS;
	static const InnertubeContext CLIENT_WEB;

	// Recommended clients - don't require PO Token (as of 2026)
	static const InnertubeContext
		CLIENT_ANDROID_SDKLESS;				  // Best: No POT needed
	static const InnertubeContext CLIENT_TV;  // Good format availability
	static const InnertubeContext CLIENT_WEB_SAFARI;  // Pre-merged HLS formats
	static const InnertubeContext CLIENT_MWEB;		  // Has ultralow formats

	// Helper to generate the JSON context payload for a request
	static nlohmann::json build_context(const InnertubeContext &client);

	// Helper to get headers
	static std::map<std::string, std::string> get_headers(
		const InnertubeContext &client);
};

}  // namespace ytdlpp::youtube
