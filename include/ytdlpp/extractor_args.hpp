#pragma once

#include <ytdlpp/ytdlpp_export.h>

#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ytdlpp::youtube {

// =============================================================================
// ExtractorArgs - Configuration system for extractor-specific arguments
//
// Implements yt-dlp's --extractor-args syntax:
//   extractor:key=value1,value2,-excluded_value
//
// Supports:
//   - Colon-separated extractor:key=values format
//   - Comma-separated values within a key
//   - Negation via minus prefix: -android_sdkless
//   - Special keys: player_client, player_skip, formats, lang
// =============================================================================

class ExtractorArgs {
   public:
	// Value type: can be a single value or a list of values
	using Value = std::vector<std::string>;

	// Constructor
	YTDLPP_EXPORT ExtractorArgs() = default;
	YTDLPP_EXPORT explicit ExtractorArgs(const std::string &args_string);

	// Parse from string (yt-dlp format)
	YTDLPP_EXPORT void parse(const std::string &args_string);

	// Check if extractor has arguments
	YTDLPP_EXPORT bool has_extractor(const std::string &extractor_name) const;

	// Get arguments for a specific extractor
	YTDLPP_EXPORT std::optional<std::map<std::string, Value>>
	get_extractor_args(const std::string &extractor_name) const;

	// Get a specific argument value
	YTDLPP_EXPORT std::optional<Value> get_arg(
		const std::string &extractor_name, const std::string &key) const;

	// Check if a value is negated (prefixed with -)
	YTDLPP_EXPORT static bool is_negated(const std::string &value);

	// Strip negation prefix from value
	YTDLPP_EXPORT static std::string strip_negation(const std::string &value);

	// Get all values for a key, handling negation
	// Returns: pair of (included_values, excluded_values)
	YTDLPP_EXPORT std::pair<std::set<std::string>, std::set<std::string>>
	get_values_with_negation(const std::string &extractor_name,
							 const std::string &key) const;

	// Special key accessors for YouTube extractor
	YTDLPP_EXPORT std::optional<std::vector<std::string>> get_player_clients()
		const;
	YTDLPP_EXPORT std::optional<std::vector<std::string>> get_player_skip()
		const;
	YTDLPP_EXPORT std::optional<std::vector<std::string>> get_formats() const;
	YTDLPP_EXPORT std::optional<std::string> get_lang() const;

	// Check if a specific client is enabled (handles negation)
	YTDLPP_EXPORT bool is_client_enabled(
		const std::string &client_name,
		const std::vector<std::string> &default_clients = {}) const;

	// Thread-safe copy
	YTDLPP_EXPORT ExtractorArgs clone() const;

	// Clear all arguments
	YTDLPP_EXPORT void clear();

	// Check if empty
	YTDLPP_EXPORT bool empty() const;

	// Copy constructor and assignment
	YTDLPP_EXPORT ExtractorArgs(const ExtractorArgs &other);
	YTDLPP_EXPORT ExtractorArgs &operator=(const ExtractorArgs &other);

	// Move constructor and assignment
	YTDLPP_EXPORT ExtractorArgs(ExtractorArgs &&other) noexcept;
	YTDLPP_EXPORT ExtractorArgs &operator=(ExtractorArgs &&other) noexcept;

   private:
	mutable std::mutex mutex_;
	// Map: extractor_name -> {key -> values}
	std::map<std::string, std::map<std::string, Value>> args_;

	// Parse a single extractor specification
	void parse_extractor_spec(const std::string &spec);

	// Parse key-value pairs (value can be comma-separated)
	std::map<std::string, Value> parse_key_values(const std::string &kv_string);
};

// =============================================================================
// YouTube-specific configuration structure
// =============================================================================

struct YouTubeConfig {
	// Player clients to use (e.g., "web", "android_sdkless", "ios")
	std::vector<std::string> player_clients;

	// Player skip options (e.g., "webpage", "configs", "js")
	std::vector<std::string> player_skip;

	// Format preferences
	std::vector<std::string> formats;

	// Language preference
	std::string lang;

	// Apply extractor args to this config
	void apply_extractor_args(const ExtractorArgs &args);

	// Get default client list
	static std::vector<std::string> default_clients();
};

}  // namespace ytdlpp::youtube
