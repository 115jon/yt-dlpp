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
	std::string platform;	  // "MOBILE", "DESKTOP"
	std::string device_make;  // "Google", "Apple"
	std::string device_model;
	// Add more fields as needed for precise emulation
};

class Innertube {
   public:
	static const InnertubeContext CLIENT_ANDROID;
	static const InnertubeContext CLIENT_IOS;
	static const InnertubeContext CLIENT_WEB;

	// Helper to generate the JSON context payload for a request
	static nlohmann::json build_context(const InnertubeContext &client);

	// Helper to get headers
	static std::map<std::string, std::string> get_headers(
		const InnertubeContext &client);
};

}  // namespace ytdlpp::youtube
