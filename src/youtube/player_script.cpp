#include "player_script.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <fstream>
#include <regex>
#include <string>

namespace ytdlpp::youtube {

namespace fs = std::filesystem;

// =============================================================================
// OPTIMIZED STATIC REGEX PATTERNS
// =============================================================================
// Pre-compiled regex patterns for player URL extraction.
// Compiled once at program startup with std::regex::optimize for better
// matching performance on large HTML pages (typically 500KB+).
// =============================================================================

namespace {

struct PlayerUrlRegexes {
	std::regex script_src;
	std::regex assets_js;
	std::regex generic_base;
	std::regex player_id;

	PlayerUrlRegexes()
		// Strategy 1: Look for <script src="...player_ias...base.js">
		: script_src(
			  R"RE(<script\s+[^>]*src="([^"]+player_ias[^"]+base\.js)"[^>]*>)RE",
			  std::regex::optimize),
		  // Strategy 2: Look for "assets": { "js": "..." }
		  assets_js(R"RE("assets"\s*:\s*\{\s*"js"\s*:\s*"([^"]+)")RE",
					std::regex::optimize),
		  // Strategy 3: Generic base.js with flexible path
		  generic_base(R"RE((/s/player/[a-zA-Z0-9._/-]+/base\.js))RE",
					   std::regex::optimize),
		  // Extract player ID from URL like /player/XXXXXX/
		  player_id(R"RE(/player/([^/]+)/)RE", std::regex::optimize) {}
};

// Get static regex instances (initialized once on first use)
const PlayerUrlRegexes &get_player_url_regexes() {
	static const PlayerUrlRegexes instance;
	return instance;
}

// =============================================================================
// STRING-BASED FALLBACK EXTRACTION
// =============================================================================
// For simple patterns, string search can be 10-50x faster than regex.
// These are used as primary methods before falling back to regex.
// =============================================================================

// Fast string-based extraction for player URL from assets JSON
std::optional<std::string> extract_assets_js_fast(const std::string &webpage) {
	// Look for "assets":{"js":"..."}
	static constexpr std::string_view marker = "\"assets\"";
	size_t pos = webpage.find(marker);
	if (pos == std::string::npos) return std::nullopt;

	// Find "js":" after assets
	static constexpr std::string_view js_marker = "\"js\":\"";
	size_t js_pos = webpage.find(js_marker, pos);
	if (js_pos == std::string::npos || js_pos > pos + 100) return std::nullopt;

	size_t url_start = js_pos + js_marker.length();
	size_t url_end = webpage.find('"', url_start);
	if (url_end == std::string::npos) return std::nullopt;

	return webpage.substr(url_start, url_end - url_start);
}

// Fast string-based extraction for /s/player/.../base.js pattern
std::optional<std::string> extract_base_js_fast(const std::string &webpage) {
	static constexpr std::string_view marker = "/s/player/";
	size_t pos = webpage.find(marker);
	if (pos == std::string::npos) return std::nullopt;

	// Find base.js after /s/player/
	static constexpr std::string_view base_marker = "base.js";
	size_t base_pos = webpage.find(base_marker, pos);
	if (base_pos == std::string::npos || base_pos > pos + 200)
		return std::nullopt;

	size_t url_end = base_pos + base_marker.length();

	// Walk back to find the URL start (either " or ')
	size_t url_start = pos;
	while (url_start > 0) {
		char c = webpage[url_start - 1];
		if (c == '"' || c == '\'' || c == ' ' || c == '=') break;
		url_start--;
	}

	return webpage.substr(url_start, url_end - url_start);
}

// Fast string-based player ID extraction from URL
std::optional<std::string> extract_player_id_fast(const std::string &url) {
	static constexpr std::string_view marker = "/player/";
	size_t pos = url.find(marker);
	if (pos == std::string::npos) return std::nullopt;

	size_t id_start = pos + marker.length();
	size_t id_end = url.find('/', id_start);
	if (id_end == std::string::npos) {
		// No trailing slash, take until end or query string
		id_end = url.find('?', id_start);
		if (id_end == std::string::npos) id_end = url.length();
	}

	if (id_end <= id_start) return std::nullopt;
	return url.substr(id_start, id_end - id_start);
}

}  // namespace

// Static member definitions
std::unordered_map<std::string, CachedPlayerData> PlayerScript::cache_;
std::mutex PlayerScript::cache_mutex_;
fs::path PlayerScript::cache_dir_ = fs::temp_directory_path() / "ytdlpp_cache";

PlayerScript::PlayerScript(net::HttpClient &http) : http_(http) {}

void PlayerScript::set_cache_directory(const fs::path &dir) {
	std::lock_guard lock(cache_mutex_);
	cache_dir_ = dir;
	std::error_code ec;
	fs::create_directories(cache_dir_, ec);
}

void PlayerScript::clear_cache() {
	std::lock_guard lock(cache_mutex_);
	cache_.clear();
	std::error_code ec;
	if (fs::exists(cache_dir_)) { fs::remove_all(cache_dir_, ec); }
}

// =============================================================================
// BYTECODE CACHING
// =============================================================================
// Bytecode is stored alongside .js files with .jsc extension.
// This provides ~10x faster script loading by skipping JS parsing.
// =============================================================================

std::optional<std::vector<uint8_t>> PlayerScript::get_cached_bytecode(
	const std::string &player_id) {
	std::lock_guard lock(cache_mutex_);

	// Check in-memory cache
	auto it = cache_.find(player_id);
	if (it != cache_.end() && it->second.bytecode) {
		spdlog::debug("Bytecode for {} found in memory cache", player_id);
		return it->second.bytecode;
	}

	// Check disk cache
	auto cache_file = cache_dir_ / (player_id + ".jsc");
	if (fs::exists(cache_file)) {
		std::ifstream file(cache_file, std::ios::binary);
		if (file) {
			std::vector<uint8_t> bytecode(
				(std::istreambuf_iterator<char>(file)),
				std::istreambuf_iterator<char>());
			// Store in memory cache
			cache_[player_id].bytecode = bytecode;
			spdlog::debug("Bytecode for {} loaded from disk ({} bytes)",
						  player_id, bytecode.size());
			return bytecode;
		}
	}

	return std::nullopt;
}

void PlayerScript::cache_bytecode(const std::string &player_id,
								  const std::vector<uint8_t> &bytecode) {
	std::lock_guard lock(cache_mutex_);

	// Store in memory
	cache_[player_id].bytecode = bytecode;

	// Store on disk
	std::error_code ec;
	fs::create_directories(cache_dir_, ec);
	auto cache_file = cache_dir_ / (player_id + ".jsc");
	std::ofstream file(cache_file, std::ios::binary);
	if (file) {
		file.write(reinterpret_cast<const char *>(bytecode.data()),
				   static_cast<std::streamsize>(bytecode.size()));
		spdlog::debug("Bytecode for {} saved to disk ({} bytes)", player_id,
					  bytecode.size());
	}
}

std::optional<std::string> PlayerScript::get_cached_script(
	const std::string &player_id) {
	std::lock_guard lock(cache_mutex_);

	// Check in-memory cache first
	auto it = cache_.find(player_id);
	if (it != cache_.end() && !it->second.script.empty()) {
		spdlog::debug("Player script {} found in memory cache", player_id);
		return it->second.script;
	}

	// Check disk cache
	auto cache_file = cache_dir_ / (player_id + ".js");
	if (fs::exists(cache_file)) {
		std::ifstream file(cache_file, std::ios::binary);
		if (file) {
			std::string content((std::istreambuf_iterator<char>(file)),
								std::istreambuf_iterator<char>());
			// Also store in memory for faster subsequent access
			cache_[player_id].script = content;
			spdlog::debug("Player script {} loaded from disk cache", player_id);
			return content;
		}
	}

	return std::nullopt;
}

void PlayerScript::cache_script(const std::string &player_id,
								const std::string &content) {
	std::lock_guard lock(cache_mutex_);

	// Store in memory
	cache_[player_id].script = content;

	// Store on disk
	std::error_code ec;
	fs::create_directories(cache_dir_, ec);
	auto cache_file = cache_dir_ / (player_id + ".js");
	std::ofstream file(cache_file, std::ios::binary);
	if (file) {
		file.write(
			content.data(), static_cast<std::streamsize>(content.size()));
		spdlog::debug("Player script {} saved to disk cache", player_id);
	}
}

std::optional<std::string> PlayerScript::extract_player_url_from_webpage(
	const std::string &webpage) {
	// =========================================================================
	// FAST STRING-BASED EXTRACTION (Primary - ~10-50x faster than regex)
	// =========================================================================

	// Try fast string-based extraction first
	if (auto result = extract_assets_js_fast(webpage)) {
		spdlog::debug("Player URL extracted via fast assets search");
		return result;
	}

	if (auto result = extract_base_js_fast(webpage)) {
		spdlog::debug("Player URL extracted via fast base.js search");
		return result;
	}

	// =========================================================================
	// REGEX FALLBACK (Slower but more robust)
	// =========================================================================
	// Only use regex if fast methods fail. Uses pre-compiled static patterns.

	const auto &regexes = get_player_url_regexes();
	std::smatch match;

	// Strategy 1: Look for <script src="...player_ias...base.js">
	try {
		if (std::regex_search(webpage, match, regexes.script_src)) {
			spdlog::debug("Player URL extracted via script_src regex");
			return match[1].str();
		}
	} catch (...) {}

	// Strategy 2: Look for "assets": { "js": "..." }
	try {
		if (std::regex_search(webpage, match, regexes.assets_js)) {
			spdlog::debug("Player URL extracted via assets_js regex");
			return match[1].str();
		}
	} catch (...) {}

	// Strategy 3: Generic base.js pattern
	try {
		if (std::regex_search(webpage, match, regexes.generic_base)) {
			spdlog::debug("Player URL extracted via generic_base regex");
			return match[1].str();
		}
	} catch (...) {}

	return std::nullopt;
}

void PlayerScript::async_fetch(const std::string &video_id, ScriptCallback cb) {
	std::string url =
		fmt::format("https://www.youtube.com/watch?v={}", video_id);

	http_.async_get(
		url,
		[this, video_id,
		 cb = std::move(cb)](Result<net::HttpResponse> res_result) mutable {
			if (res_result.has_error()) {
				spdlog::error("Failed to fetch video page: {}",
							  res_result.error().message());
				return cb(std::nullopt);
			}
			auto res = res_result.value();
			if (res.status_code != 200) {
				spdlog::error(
					"Failed to fetch video page. Status: {}", res.status_code);
				return cb(std::nullopt);
			}

			auto extracted_url = extract_player_url_from_webpage(res.body);
			if (!extracted_url) {
				spdlog::error("Failed to extract player URL");
				return cb(std::nullopt);
			}

			player_url_ = *extracted_url;

			// Extract player ID using fast string method first
			std::string player_id = "unknown";
			if (auto id = extract_player_id_fast(player_url_)) {
				player_id = *id;
			} else {
				// Fallback to regex for unusual URL formats
				const auto &regexes = get_player_url_regexes();
				try {
					std::smatch m;
					if (std::regex_search(player_url_, m, regexes.player_id)) {
						player_id = m[1].str();
					}
				} catch (...) {}
			}

			// Check cache before downloading
			auto cached = get_cached_script(player_id);
			if (cached) {
				spdlog::info("{}: Using cached player {}", video_id, player_id);
				return cb(*cached);
			}

			spdlog::info("{}: Downloading player {}", video_id, player_id);

			if (player_url_.find("http") != 0) {
				if (player_url_[0] == '/') {
					player_url_ = "https://www.youtube.com" + player_url_;
				} else {
					player_url_ = "https://www.youtube.com/" + player_url_;
				}
			}

			http_.async_get(
				player_url_,
				[this, player_id, cb = std::move(cb)](
					Result<net::HttpResponse> script_res_result) mutable {
					if (script_res_result.has_error()) {
						spdlog::error("Failed to download player script: {}",
									  script_res_result.error().message());
						return cb(std::nullopt);
					}
					auto script_res = script_res_result.value();
					if (script_res.status_code != 200) {
						spdlog::error("Failed to download player script");
						return cb(std::nullopt);
					}
					// Cache the script
					cache_script(player_id, script_res.body);
					cb(script_res.body);
				});
		},
		{{"User-Agent",
		  "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
		  "(KHTML, like Gecko) Chrome/121.0.0.0 Safari/537.36"}});
}

}  // namespace ytdlpp::youtube
