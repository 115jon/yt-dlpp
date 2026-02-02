#include <fmt/ranges.h>
#include <spdlog/spdlog.h>

#include <boost/algorithm/string.hpp>
#include <ytdlpp/extractor_args.hpp>

namespace ytdlpp::youtube {

// =============================================================================
// Helper Functions
// =============================================================================

bool ExtractorArgs::is_negated(const std::string &value) {
	return !value.empty() && value[0] == '-';
}

std::string ExtractorArgs::strip_negation(const std::string &value) {
	if (is_negated(value)) { return value.substr(1); }
	return value;
}

// =============================================================================
// Constructor and Parsing
// =============================================================================

ExtractorArgs::ExtractorArgs(const std::string &args_string) {
	parse(args_string);
}

void ExtractorArgs::parse(const std::string &args_string) {
	std::lock_guard<std::mutex> lock(mutex_);
	args_.clear();

	if (args_string.empty()) { return; }

	// Split by semicolon for multiple extractors
	std::vector<std::string> extractor_specs;
	boost::split(extractor_specs, args_string, boost::is_any_of(";"));

	for (auto &spec : extractor_specs) {
		boost::trim(spec);
		if (!spec.empty()) { parse_extractor_spec(spec); }
	}
}

void ExtractorArgs::parse_extractor_spec(const std::string &spec) {
	// yt-dlp Format: extractor:key=value1,value2,-value3
	// Note: Commas within values separate multiple values for the SAME key
	// Multiple keys use separate --extractor-args invocations

	auto colon_pos = spec.find(':');
	if (colon_pos == std::string::npos) {
		spdlog::warn("Invalid extractor spec (missing colon): {}", spec);
		return;
	}

	std::string extractor_name = spec.substr(0, colon_pos);
	std::string kv_part = spec.substr(colon_pos + 1);
	boost::trim(extractor_name);
	boost::trim(kv_part);

	if (extractor_name.empty() || kv_part.empty()) {
		spdlog::warn("Invalid extractor spec (empty parts): {}", spec);
		return;
	}

	boost::to_lower(extractor_name);
	args_[extractor_name] = parse_key_values(kv_part);
}

std::map<std::string, ExtractorArgs::Value> ExtractorArgs::parse_key_values(
	const std::string &kv_string) {
	std::map<std::string, Value> result;

	// Find the first '=' to get the key
	auto eq_pos = kv_string.find('=');
	if (eq_pos == std::string::npos) {
		// Key without value (boolean flag)
		std::string key = kv_string;
		boost::trim(key);
		boost::to_lower(key);
		result[key] = {"true"};
		return result;
	}

	std::string key = kv_string.substr(0, eq_pos);
	std::string value_str = kv_string.substr(eq_pos + 1);
	boost::trim(key);
	boost::trim(value_str);
	boost::to_lower(key);

	// yt-dlp format: values are comma-separated
	// e.g., player_client=default,-android_sdkless,web
	Value values;
	std::vector<std::string> value_parts;
	boost::split(value_parts, value_str, boost::is_any_of(","));

	for (auto &v : value_parts) {
		boost::trim(v);
		if (!v.empty()) { values.push_back(v); }
	}

	if (!values.empty()) { result[key] = std::move(values); }
	return result;
}

// =============================================================================
// Accessors
// =============================================================================

bool ExtractorArgs::has_extractor(const std::string &extractor_name) const {
	std::lock_guard<std::mutex> lock(mutex_);
	std::string lower_name = boost::to_lower_copy(extractor_name);
	return args_.find(lower_name) != args_.end();
}

std::optional<std::map<std::string, ExtractorArgs::Value>>
ExtractorArgs::get_extractor_args(const std::string &extractor_name) const {
	std::lock_guard<std::mutex> lock(mutex_);
	std::string lower_name = boost::to_lower_copy(extractor_name);

	auto it = args_.find(lower_name);
	if (it != args_.end()) { return it->second; }
	return std::nullopt;
}

std::optional<ExtractorArgs::Value> ExtractorArgs::get_arg(
	const std::string &extractor_name, const std::string &key) const {
	std::lock_guard<std::mutex> lock(mutex_);
	std::string lower_name = boost::to_lower_copy(extractor_name);
	std::string lower_key = boost::to_lower_copy(key);

	auto ext_it = args_.find(lower_name);
	if (ext_it != args_.end()) {
		auto key_it = ext_it->second.find(lower_key);
		if (key_it != ext_it->second.end()) { return key_it->second; }
	}
	return std::nullopt;
}

std::pair<std::set<std::string>, std::set<std::string>>
ExtractorArgs::get_values_with_negation(const std::string &extractor_name,
										const std::string &key) const {
	std::set<std::string> included;
	std::set<std::string> excluded;

	auto values_opt = get_arg(extractor_name, key);
	if (!values_opt) { return {included, excluded}; }

	for (const auto &value : *values_opt) {
		if (is_negated(value)) {
			excluded.insert(strip_negation(value));
		} else {
			included.insert(value);
		}
	}

	return {included, excluded};
}

// =============================================================================
// YouTube-specific Accessors
// =============================================================================

std::optional<std::vector<std::string>> ExtractorArgs::get_player_clients()
	const {
	auto [included, excluded] =
		get_values_with_negation("youtube", "player_client");

	spdlog::debug("get_player_clients: included=[{}], excluded=[{}]",
				  fmt::join(included, ", "), fmt::join(excluded, ", "));

	std::vector<std::string> result;

	// Check if "default" is in included values - expand it to default clients
	bool has_default = included.find("default") != included.end();
	if (has_default) {
		included.erase("default");
		// Add default clients in priority order
		for (const auto &c : YouTubeConfig::default_clients()) {
			result.push_back(c);
		}
	}

	// Add explicitly included clients (after defaults if 'default' was used)
	for (const auto &c : included) {
		// Avoid duplicates
		if (std::find(result.begin(), result.end(), c) == result.end()) {
			result.push_back(c);
		}
	}

	// If nothing included (and no default keyword), use defaults
	if (result.empty()) { result = YouTubeConfig::default_clients(); }

	spdlog::debug(
		"get_player_clients: before exclusion=[{}]", fmt::join(result, ", "));

	// Remove excluded clients using erase-remove idiom
	for (const auto &excl : excluded) {
		result.erase(
			std::remove(result.begin(), result.end(), excl), result.end());
	}

	spdlog::debug(
		"get_player_clients: after exclusion=[{}]", fmt::join(result, ", "));

	if (result.empty()) { return std::nullopt; }
	return result;
}

std::optional<std::vector<std::string>> ExtractorArgs::get_player_skip() const {
	auto values_opt = get_arg("youtube", "player_skip");
	if (!values_opt) { return std::nullopt; }
	return *values_opt;
}

std::optional<std::vector<std::string>> ExtractorArgs::get_formats() const {
	auto values_opt = get_arg("youtube", "formats");
	if (!values_opt) { return std::nullopt; }
	return *values_opt;
}

std::optional<std::string> ExtractorArgs::get_lang() const {
	auto values_opt = get_arg("youtube", "lang");
	if (!values_opt || values_opt->empty()) { return std::nullopt; }
	return values_opt->front();
}

bool ExtractorArgs::is_client_enabled(
	const std::string &client_name,
	const std::vector<std::string> &default_clients) const {
	auto clients_opt = get_player_clients();
	if (!clients_opt) {
		// Check if client is in defaults and not excluded
		auto [included, excluded] =
			get_values_with_negation("youtube", "player_client");

		// If no player_client specified at all, use defaults
		if (included.empty() && excluded.empty()) {
			return std::find(default_clients.begin(), default_clients.end(),
							 client_name) != default_clients.end();
		}

		// Check if explicitly excluded
		return excluded.find(client_name) == excluded.end();
	}

	return std::find(clients_opt->begin(), clients_opt->end(), client_name) !=
		   clients_opt->end();
}

// =============================================================================
// Utility Methods
// =============================================================================

ExtractorArgs::ExtractorArgs(const ExtractorArgs &other) : args_(other.args_) {
	std::lock_guard<std::mutex> lock(other.mutex_);
}

ExtractorArgs &ExtractorArgs::operator=(const ExtractorArgs &other) {
	if (this != &other) {
		std::lock_guard<std::mutex> lock_this(mutex_);
		std::lock_guard<std::mutex> lock_other(other.mutex_);
		args_ = other.args_;
	}
	return *this;
}

ExtractorArgs::ExtractorArgs(ExtractorArgs &&other) noexcept
	: args_(std::move(other.args_)) {
	std::lock_guard<std::mutex> lock(other.mutex_);
}

ExtractorArgs &ExtractorArgs::operator=(ExtractorArgs &&other) noexcept {
	if (this != &other) {
		std::lock_guard<std::mutex> lock_this(mutex_);
		std::lock_guard<std::mutex> lock_other(other.mutex_);
		args_ = std::move(other.args_);
	}
	return *this;
}

ExtractorArgs ExtractorArgs::clone() const { return ExtractorArgs(*this); }

void ExtractorArgs::clear() {
	std::lock_guard<std::mutex> lock(mutex_);
	args_.clear();
}

bool ExtractorArgs::empty() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return args_.empty();
}

// =============================================================================
// YouTubeConfig Implementation
// =============================================================================

std::vector<std::string> YouTubeConfig::default_clients() {
	// Match yt-dlp 2026 defaults
	return {"android_sdkless", "web", "web_safari"};
}

void YouTubeConfig::apply_extractor_args(const ExtractorArgs &args) {
	// Apply player clients
	auto clients_opt = args.get_player_clients();
	if (clients_opt) {
		player_clients = *clients_opt;
		spdlog::debug("YouTubeConfig: Using player clients: [{}]",
					  fmt::join(player_clients, ", "));
	}

	// Apply player skip
	auto skip_opt = args.get_player_skip();
	if (skip_opt) {
		player_skip = *skip_opt;
		spdlog::debug(
			"YouTubeConfig: Player skip: [{}]", fmt::join(player_skip, ", "));
	}

	// Apply formats
	auto formats_opt = args.get_formats();
	if (formats_opt) {
		formats = *formats_opt;
		spdlog::debug("YouTubeConfig: Formats: [{}]", fmt::join(formats, ", "));
	}

	// Apply language
	auto lang_opt = args.get_lang();
	if (lang_opt) {
		lang = *lang_opt;
		spdlog::debug("YouTubeConfig: Language: {}", lang);
	}
}

}  // namespace ytdlpp::youtube
