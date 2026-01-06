#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../../include/ytdlpp/http_client.hpp"

namespace ytdlpp::youtube {

// =============================================================================
// CACHED PLAYER SCRIPT
// =============================================================================
// Stores both raw JavaScript and pre-compiled bytecode for optimal performance.
// On first load, JS is parsed normally. On subsequent loads, bytecode is used
// which is ~10x faster than re-parsing the ~1MB player script.
// =============================================================================

struct CachedPlayerData {
	std::string script;							   // Raw JavaScript source
	std::optional<std::vector<uint8_t>> bytecode;  // Pre-compiled bytecode
};

class PlayerScript {
   public:
	explicit PlayerScript(ytdlpp::net::HttpClient &http);

	using ScriptCallback =
		boost::asio::any_completion_handler<void(std::optional<std::string>)>;

	// Async fetch with bytecode support
	// Returns script content and caches bytecode for next load
	void async_fetch(const std::string &video_id, ScriptCallback cb);

	std::string get_captured_player_url() const { return player_url_; }

	// Cache control
	static void set_cache_directory(const std::filesystem::path &dir);
	static void clear_cache();

	// Get cached bytecode for a player (used by SigDecipherer)
	static std::optional<std::vector<uint8_t>> get_cached_bytecode(
		const std::string &player_id);

	// Store bytecode after successful compilation (used by SigDecipherer)
	static void cache_bytecode(const std::string &player_id,
							   const std::vector<uint8_t> &bytecode);

   private:
	ytdlpp::net::HttpClient &http_;
	std::string player_url_;

	std::optional<std::string> extract_player_url_from_webpage(
		const std::string &webpage);

	// In-memory cache (player_id -> cached data)
	static std::unordered_map<std::string, CachedPlayerData> cache_;
	static std::mutex cache_mutex_;
	static std::filesystem::path cache_dir_;

	std::optional<std::string> get_cached_script(const std::string &player_id);
	void cache_script(const std::string &player_id, const std::string &content);
};

}  // namespace ytdlpp::youtube
