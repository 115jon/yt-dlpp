#include "player_script.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <regex>
#include <string>

namespace ytdlpp::youtube {

PlayerScript::PlayerScript(net::HttpClient &http) : http_(http) {}

std::optional<std::string> PlayerScript::fetch(const std::string &video_id) {
	// std::string url = fmt::format("https://www.youtube.com/watch?v={}",
	// video_id); Already logged in Extractor "Downloading webpage"
	std::string url =
		fmt::format("https://www.youtube.com/watch?v={}", video_id);

	auto res_result = http_.get(
		url, {{"User-Agent",
			   "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
			   "(KHTML, like Gecko) Chrome/121.0.0.0 Safari/537.36"}});

	if (res_result.has_error()) {
		spdlog::error(
			"Failed to fetch video page: {}", res_result.error().message());
		return std::nullopt;
	}
	auto res = res_result.value();

	if (res.status_code != 200) {
		spdlog::error(
			"Failed to fetch video page. Status: {}", res.status_code);
		return std::nullopt;
	}

	auto extracted_url = extract_player_url_from_webpage(res.body);
	if (!extracted_url) {
		spdlog::error("Failed to extract player URL");
		return std::nullopt;
	}

	player_url_ = *extracted_url;
	// Extract player ID from URL for logging
	// URL: /s/player/50cc0679/player_ias.vflset/en_US/base.js -> 50cc0679
	// or /s/player/50cc0679/base.js
	// Regex to grab the component after /player/
	std::string player_id = "unknown";
	try {
		std::regex id_re("/player/([^/]+)/");
		std::smatch m;
		if (std::regex_search(player_url_, m, id_re)) {
			player_id = m[1].str();
		}
	} catch (...) {}

	spdlog::info("{}: Downloading player {}", video_id, player_id);

	if (player_url_.find("http") != 0) {
		if (player_url_[0] == '/') {
			player_url_ = "https://www.youtube.com" + player_url_;
		} else {
			player_url_ = "https://www.youtube.com/" + player_url_;
		}
	}

	// spdlog::info("Downloading player script from: {}", player_url_); //
	// Redundant, matches yt-dlp trace log instead
	auto script_res_result = http_.get(player_url_);
	if (script_res_result.has_error()) {
		spdlog::error("Failed to download player script: {}",
					  script_res_result.error().message());
		return std::nullopt;
	}
	auto script_res = script_res_result.value();

	if (script_res.status_code != 200) {
		spdlog::error("Failed to download player script");
		return std::nullopt;
	}

	return script_res.body;
}

std::optional<std::string> PlayerScript::extract_player_url_from_webpage(
	const std::string &webpage) {
	std::smatch match;

	// Strategy 1: Look for <script src="...">
	try {
		std::regex script_re(
			"<script\\s+[^>]*src=\"([^\"]+player_ias[^\"]+base\\.js)\"[^>]*>");
		if (std::regex_search(webpage, match, script_re)) {
			return match[1].str();
		}
	} catch (...) {}

	// Strategy 2: Look for ytcfg.set({ ... "assets": { "js": "..." }
	try {
		std::regex assets_re(
			"\"assets\"\\s*:\\s*\\{\\s*\"js\"\\s*:\\s*\"([^\"]+)\"");
		if (std::regex_search(webpage, match, assets_re)) {
			return match[1].str();
		}
	} catch (...) {}

	// Strategy 3: Try finding generic base.js with flexible path
	try {
		std::regex generic_base("(/s/player/[a-zA-Z0-9\\._\\/-]+/base\\.js)");
		if (std::regex_search(webpage, match, generic_base)) {
			return match[1].str();
		}
	} catch (...) {}

	return std::nullopt;
}

}  // namespace ytdlpp::youtube
