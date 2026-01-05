#include "innertube.hpp"

namespace ytdlpp::youtube {

// Constants mirroring yt-dlp _base.py as of recent versions
// Note: These change often. In a real project, we might load these from a
// config file.

const InnertubeContext Innertube::CLIENT_ANDROID = {
	"ANDROID",
	"19.05.35",	 // Modern version
	"com.google.android.youtube/19.05.35 (Linux; U; Android 11; en_US; Pixel "
	"5)",
	"Android",
	"11",
	"MOBILE",
	"Google",
	"Pixel 5"};

const InnertubeContext Innertube::CLIENT_IOS = {
	"IOS",
	"19.05.3",	// Modern version
	"com.google.ios.youtube/19.05.3 (iPhone14,5; U; CPU iOS 17_3 like Mac OS "
	"X; en_US)",
	"iOS",
	"17.3",
	"MOBILE",
	"Apple",
	"iPhone14,5"};

const InnertubeContext Innertube::CLIENT_WEB = {
	"WEB",
	"2.20240217.09.00",
	"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like "
	"Gecko) Chrome/121.0.0.0 Safari/537.36",
	"Windows",
	"10.0",
	"DESKTOP",
	"",
	""};

nlohmann::json Innertube::build_context(const InnertubeContext &client) {
	nlohmann::json ctx = {
		{"context",
		 {{"client",
		   {{"clientName", client.client_name},
			{"clientVersion", client.client_version},
			{"osName", client.os_name},
			{"osVersion", client.os_version},
			{"platform", client.platform},
			{"hl", "en"},
			{"gl", "US"},
			{"timeZone", "UTC"}}}}}};

	if (!client.device_make.empty()) {
		ctx["context"]["client"]["deviceMake"] = client.device_make;
	}
	if (!client.device_model.empty()) {
		ctx["context"]["client"]["deviceModel"] = client.device_model;
	}

	return ctx;
}

std::map<std::string, std::string> Innertube::get_headers(
	const InnertubeContext &client) {
	return {{"User-Agent", client.user_agent},
			{"Content-Type", "application/json"},
			{"X-Goog-Api-Format-Version", "1"}};
}

}  // namespace ytdlpp::youtube
