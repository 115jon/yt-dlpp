#include "innertube.hpp"

namespace ytdlpp::youtube {

// Client priority: android_sdkless (no POT) > tv > web_safari (HLS) > web

// ANDROID client - standard Android app
// Note: May require PO Token for some videos
const InnertubeContext Innertube::CLIENT_ANDROID = {
	"ANDROID",
	"20.10.38",	 // Updated from yt-dlp 2026
	"com.google.android.youtube/20.10.38 (Linux; U; Android 11) gzip",
	"Android",
	"11",
	"MOBILE",
	"Google",
	"Pixel 5",
	3};	 // INNERTUBE_CONTEXT_CLIENT_NAME = 3

// IOS client - iPhone app
// Has HLS live streams, 60fps formats on newer devices
const InnertubeContext Innertube::CLIENT_IOS = {
	"IOS",
	"20.10.4",	// Updated from yt-dlp 2026
	"com.google.ios.youtube/20.10.4 (iPhone16,2; U; CPU iOS 18_3_2 like Mac OS "
	"X;)",
	"iPhone",
	"18.3.2.22D82",
	"MOBILE",
	"Apple",
	"iPhone16,2",
	5};	 // INNERTUBE_CONTEXT_CLIENT_NAME = 5

// WEB client - Standard web browser
// Requires JS player for signature deciphering
const InnertubeContext Innertube::CLIENT_WEB = {
	"WEB",
	"2.20250925.01.00",	 // Updated from yt-dlp 2026
	"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like "
	"Gecko) Chrome/121.0.0.0 Safari/537.36",
	"Windows",
	"10.0",
	"DESKTOP",
	"",
	"",
	1};	 // INNERTUBE_CONTEXT_CLIENT_NAME = 1

// Recommended clients - no PO Token required

// ANDROID_SDKLESS - Android without SDK checks
// BEST CHOICE: Doesn't require PO Token for most videos!
const InnertubeContext Innertube::CLIENT_ANDROID_SDKLESS = {
	"ANDROID",	// Same clientName as ANDROID
	"20.10.38",
	"com.google.android.youtube/20.10.38 (Linux; U; Android 11) gzip",
	"Android",
	"11",
	"MOBILE",
	"",	 // No deviceMake (key difference from CLIENT_ANDROID)
	"",
	3};	 // INNERTUBE_CONTEXT_CLIENT_NAME = 3

// TV client - Smart TV / Cobalt browser
// Good format availability, works for most videos
const InnertubeContext Innertube::CLIENT_TV = {
	"TVHTML5",
	"7.20250923.13.00",	 // From yt-dlp 2026
	"Mozilla/5.0 (ChromiumStylePlatform) Cobalt/Version",
	"",	 // No osName for TV
	"",
	"TV",
	"",
	"",
	7};	 // INNERTUBE_CONTEXT_CLIENT_NAME = 7

// WEB_SAFARI - Safari browser user agent
// Returns pre-merged video+audio HLS formats (144p/240p/360p/720p/1080p)
const InnertubeContext Innertube::CLIENT_WEB_SAFARI = {
	"WEB",
	"2.20250925.01.00",
	"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
	"(KHTML, like Gecko) Version/15.5 Safari/605.1.15,gzip(gfe)",
	"Macintosh",
	"10.15.7",
	"DESKTOP",
	"Apple",
	"Macintosh",
	1};	 // INNERTUBE_CONTEXT_CLIENT_NAME = 1

// MWEB client - Mobile web
// Has 'ultralow' formats, previously worked without PO Token with iPad UA
const InnertubeContext Innertube::CLIENT_MWEB = {
	"MWEB",
	"2.20250925.01.00",
	"Mozilla/5.0 (iPad; CPU OS 16_7_10 like Mac OS X) AppleWebKit/605.1.15 "
	"(KHTML, like Gecko) Version/16.6 Mobile/15E148 Safari/604.1,gzip(gfe)",
	"iPad",
	"16.7.10",
	"MOBILE",
	"Apple",
	"iPad",
	2};	 // INNERTUBE_CONTEXT_CLIENT_NAME = 2

nlohmann::json Innertube::build_context(const InnertubeContext &client,
										const std::string &visitor_data,
										const std::string &po_token) {
	nlohmann::json ctx = {
		{"context",
		 {{"client",
		   {{"clientName", client.client_name},
			{"clientVersion", client.client_version},
			{"hl", "en"},
			{"gl", "US"},
			{"timeZone", "UTC"}}}}}};

	// Only add non-empty fields
	if (!client.os_name.empty()) {
		ctx["context"]["client"]["osName"] = client.os_name;
	}
	if (!client.os_version.empty()) {
		ctx["context"]["client"]["osVersion"] = client.os_version;
	}
	if (!client.platform.empty()) {
		ctx["context"]["client"]["platform"] = client.platform;
	}
	if (!client.device_make.empty()) {
		ctx["context"]["client"]["deviceMake"] = client.device_make;
	}
	if (!client.device_model.empty()) {
		ctx["context"]["client"]["deviceModel"] = client.device_model;
	}

	// Add userAgent if not empty (mobile clients benefit from this)
	if (!client.user_agent.empty()) {
		ctx["context"]["client"]["userAgent"] = client.user_agent;
	}

	if (!visitor_data.empty()) {
		ctx["context"]["client"]["visitorData"] = visitor_data;
	}

	if (!po_token.empty()) {
		ctx["context"]["serviceIntegrityDimensions"]["poToken"] = po_token;
	}

	return ctx;
}

std::map<std::string, std::string> Innertube::get_headers(
	const InnertubeContext &client) {
	return {{"User-Agent", client.user_agent},
			{"Content-Type", "application/json"},
			{"X-YouTube-Client-Name", std::to_string(client.client_id)},
			{"X-YouTube-Client-Version", client.client_version},
			{"X-Goog-Api-Format-Version", "1"},
			{"Origin", "https://www.youtube.com"}};
}

}  // namespace ytdlpp::youtube
