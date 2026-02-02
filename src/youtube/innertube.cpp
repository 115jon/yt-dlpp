#include "innertube.hpp"

namespace ytdlpp::youtube {

// Client priority: android_sdkless (no POT) > tv > web_safari (HLS) > web

// ANDROID client - standard Android app
// Note: May require PO Token for some videos
const InnertubeContext Innertube::CLIENT_ANDROID = {
	"ANDROID",
	"21.02.35",	 // Updated from yt-dlp 2026-01-18
	"com.google.android.youtube/21.02.35 (Linux; U; Android 11) gzip",
	"Android",
	"11",
	"MOBILE",
	"Google",
	"Pixel 5",
	3,	 // INNERTUBE_CONTEXT_CLIENT_NAME = 3
	30,	 // androidSdkVersion
	"www.youtube.com"};

// IOS client - iPhone app
// Has HLS live streams, 60fps formats on newer devices
const InnertubeContext Innertube::CLIENT_IOS = {
	"IOS",
	"21.02.3",	// Updated from yt-dlp 2026-01-18
	"com.google.ios.youtube/21.02.3 (iPhone16,2; U; CPU iOS 18_3_2 like Mac OS "
	"X;)",
	"iPhone",
	"18.3.2.22D82",
	"MOBILE",
	"Apple",
	"iPhone16,2",
	5,	// INNERTUBE_CONTEXT_CLIENT_NAME = 5
	0,	// androidSdkVersion (not applicable for iOS)
	"www.youtube.com"};

// WEB client - Standard web browser
// Requires JS player for signature deciphering
const InnertubeContext Innertube::CLIENT_WEB = {
	"WEB",
	"2.20260114.08.00",	 // Updated from yt-dlp 2026-01-18
	"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like "
	"Gecko) Chrome/121.0.0.0 Safari/537.36",
	"Windows",
	"10.0",
	"DESKTOP",
	"",
	"",
	1,	// INNERTUBE_CONTEXT_CLIENT_NAME = 1
	0,	// androidSdkVersion (not applicable for web)
	"www.youtube.com"};

// ANDROID_VR client - VR app
// Highly reliable workaround for 403 errors (used by OuterTune)
const InnertubeContext Innertube::CLIENT_ANDROID_VR = {
	"ANDROID_VR",
	"1.61.48",
	"com.google.android.apps.youtube.vr.oculus/1.61.48 (Linux; U; Android 12; "
	"eureka-user Build/SQ3A.220605.009.A1) gzip",
	"Android",
	"12",
	"MOBILE",
	"Oculus",
	"Quest 3",
	28,	 // INNERTUBE_CONTEXT_CLIENT_NAME = 28
	31,	 // androidSdkVersion
	"www.youtube.com"};

// Recommended clients - no PO Token required

// ANDROID_SDKLESS - Android without SDK checks
// BEST CHOICE: Doesn't require PO Token for most videos!
const InnertubeContext Innertube::CLIENT_ANDROID_SDKLESS = {
	"ANDROID",	 // Same clientName as ANDROID
	"21.02.35",	 // Updated from yt-dlp 2026-01-18
	"com.google.android.youtube/21.02.35 (Linux; U; Android 11) gzip",
	"Android",
	"11",
	"MOBILE",
	"",	 // No deviceMake (key difference from CLIENT_ANDROID)
	"",
	3,	 // INNERTUBE_CONTEXT_CLIENT_NAME = 3
	30,	 // androidSdkVersion
	"www.youtube.com"};

// TV client - Smart TV / Cobalt browser
// Good format availability, works for most videos
const InnertubeContext Innertube::CLIENT_TV = {
	"TVHTML5",
	"7.20260114.12.00",	 // Updated from yt-dlp 2026-01-18
	"Mozilla/5.0 (ChromiumStylePlatform) Cobalt/Version",
	"",	 // No osName for TV
	"",
	"TV",
	"",
	"",
	7,	// INNERTUBE_CONTEXT_CLIENT_NAME = 7
	0,	// androidSdkVersion (not applicable for TV)
	"www.youtube.com"};

// WEB_SAFARI - Safari browser user agent
// Returns pre-merged video+audio HLS formats (144p/240p/360p/720p/1080p)
const InnertubeContext Innertube::CLIENT_WEB_SAFARI = {
	"WEB",
	"2.20260114.08.00",	 // Updated from yt-dlp 2026-01-18
	"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
	"(KHTML, like Gecko) Version/15.5 Safari/605.1.15,gzip(gfe)",
	"Macintosh",
	"10.15.7",
	"DESKTOP",
	"Apple",
	"Macintosh",
	1,	// INNERTUBE_CONTEXT_CLIENT_NAME = 1
	0,	// androidSdkVersion (not applicable for web)
	"www.youtube.com"};

// MWEB client - Mobile web
// Has 'ultralow' formats, previously worked without PO Token with iPad UA
const InnertubeContext Innertube::CLIENT_MWEB = {
	"MWEB",
	"2.20260115.01.00",	 // Updated from yt-dlp 2026-01-18
	"Mozilla/5.0 (iPad; CPU OS 16_7_10 like Mac OS X) AppleWebKit/605.1.15 "
	"(KHTML, like Gecko) Version/16.6 Mobile/15E148 Safari/604.1,gzip(gfe)",
	"iPad",
	"16.7.10",
	"MOBILE",
	"Apple",
	"iPad",
	2,	// INNERTUBE_CONTEXT_CLIENT_NAME = 2
	0,	// androidSdkVersion (not applicable for MWEB)
	"www.youtube.com"};

// WEB_CREATOR client - Creator Studio
const InnertubeContext Innertube::CLIENT_WEB_CREATOR = {
	"WEB_CREATOR",
	"1.20260114.08.00",
	"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like "
	"Gecko) Chrome/121.0.0.0 Safari/537.36",
	"Windows",
	"10.0",
	"DESKTOP",
	"",
	"",
	62,	 // INNERTUBE_CONTEXT_CLIENT_NAME = 62
	0,	 // androidSdkVersion (not applicable for web)
	"www.youtube.com"};

// YouTube Music client
const InnertubeContext Innertube::CLIENT_WEB_MUSIC = {
	"WEB_REMIX",
	"1.20260114.03.00",
	"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like "
	"Gecko) Chrome/121.0.0.0 Safari/537.36",
	"Windows",
	"10.0",
	"DESKTOP",
	"",
	"",
	67,	 // INNERTUBE_CONTEXT_CLIENT_NAME = 67
	0,
	"music.youtube.com"};

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
			{"utcOffsetMinutes", 0},
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

	// Add androidSdkVersion for Android clients
	if (client.android_sdk_version > 0) {
		ctx["context"]["client"]["androidSdkVersion"] =
			client.android_sdk_version;
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
	const InnertubeContext &client, const std::string &visitor_data) {
	auto headers = std::map<std::string, std::string>{
		{"User-Agent", client.user_agent},
		{"Content-Type", "application/json"},
		{"Accept", "application/json"},
		{"X-YouTube-Client-Name", std::to_string(client.client_id)},
		{"X-YouTube-Client-Version", client.client_version},
		{"X-Goog-Api-Format-Version", "2"},
		{"Origin", "https://" + client.api_host},
		{"Referer", "https://" + client.api_host + "/"}};

	// Add visitor ID for all clients if available (required for GVS token
	// validation)
	if (!visitor_data.empty()) { headers["X-Goog-Visitor-Id"] = visitor_data; }

	return headers;
}

}  // namespace ytdlpp::youtube
