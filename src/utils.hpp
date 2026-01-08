#pragma once

#include <boost/charconv.hpp>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <ytdlpp/result.hpp>

namespace ytdlpp::utils {

// =============================================================================
// Safe numeric conversions utilizing boost::charconv
// =============================================================================

template <typename T>
Result<T> to_number(std::string_view sv) {
	T val;
	auto res =
		boost::charconv::from_chars(sv.data(), sv.data() + sv.size(), val);
	if (res.ec == std::errc{}) { return val; }
	return make_error_code(errc::invalid_number_format);
}

// Wrappers for common uses
inline Result<int> to_int(std::string_view sv) { return to_number<int>(sv); }

inline Result<long long> to_long(std::string_view sv) {
	return to_number<long long>(sv);
}

inline Result<double> to_double(std::string_view sv) {
	return to_number<double>(sv);
}

// Fallback helper for default values commonly used in JSON parsing where
// 0/empty string is preferred over error
template <typename T>
T to_number_default(std::string_view sv, T def_val = 0) {
	T val;
	auto res =
		boost::charconv::from_chars(sv.data(), sv.data() + sv.size(), val);
	if (res.ec == std::errc{}) { return val; }
	return def_val;
}

// =============================================================================
// JSON Traversal Utilities (similar to yt-dlp's traverse_obj)
// =============================================================================

// PathElement wrapper to handle both string keys and integer indices
// Supports implicit conversion from const char*, std::string, and int
class PathElement {
   public:
	// Constructors for implicit conversion
	PathElement(const char *key) : m_is_index(false), m_key(key), m_index(0) {}
	PathElement(const std::string &key)
		: m_is_index(false), m_key(key), m_index(0) {}
	PathElement(std::string &&key)
		: m_is_index(false), m_key(std::move(key)), m_index(0) {}
	PathElement(int index) : m_is_index(true), m_index(index) {}

	[[nodiscard]] bool is_index() const { return m_is_index; }
	[[nodiscard]] const std::string &key() const { return m_key; }
	[[nodiscard]] int index() const { return m_index; }

   private:
	bool m_is_index;
	std::string m_key;
	int m_index;
};

namespace detail {

// Navigate one step in the JSON structure
inline const nlohmann::json *step(const nlohmann::json *j,
								  const PathElement &elem) {
	if (!j) return nullptr;

	if (!elem.is_index()) {
		const auto &key = elem.key();
		if (j->is_object() && j->contains(key)) { return &(*j)[key]; }
	} else {
		int idx = elem.index();
		if (j->is_array()) {
			// Support negative indexing like Python
			if (idx < 0) { idx = static_cast<int>(j->size()) + idx; }
			if (idx >= 0 && static_cast<size_t>(idx) < j->size()) {
				return &(*j)[static_cast<size_t>(idx)];
			}
		}
	}
	return nullptr;
}

// Recursively traverse the path
inline const nlohmann::json *traverse(
	const nlohmann::json *j, const std::initializer_list<PathElement> &path) {
	for (const auto &elem : path) {
		j = step(j, elem);
		if (!j) return nullptr;
	}
	return j;
}

}  // namespace detail

/// Traverse a JSON object using a path of keys/indices.
/// Returns std::nullopt if path doesn't exist or value can't be converted.
///
/// Usage:
///   auto val = traverse_obj<std::string>(json, {"videoDetails", "title"});
///   auto item = traverse_obj<nlohmann::json>(json, {"items", 0});
///   auto last = traverse_obj<std::string>(json, {"items", -1, "name"}); //
///   negative index
template <typename T>
std::optional<T> traverse_obj(const nlohmann::json &j,
							  std::initializer_list<PathElement> path) {
	const nlohmann::json *result = detail::traverse(&j, path);
	if (!result) return std::nullopt;

	try {
		return result->get<T>();
	} catch (...) { return std::nullopt; }
}

/// Traverse and return the raw JSON node (useful for arrays/objects)
inline std::optional<nlohmann::json> traverse_json(
	const nlohmann::json &j, std::initializer_list<PathElement> path) {
	const nlohmann::json *result = detail::traverse(&j, path);
	if (!result) return std::nullopt;
	return *result;
}

/// Traverse with a default value (never returns nullopt)
template <typename T>
T traverse_obj_default(const nlohmann::json &j,
					   std::initializer_list<PathElement> path, T default_val) {
	auto result = traverse_obj<T>(j, path);
	return result.value_or(std::move(default_val));
}

/// Get text from runs array (common YouTube pattern)
/// Handles: {"runs": [{"text": "hello"}, {"text": " world"}]} -> "hello world"
inline std::string get_text_from_runs(const nlohmann::json &j,
									  std::initializer_list<PathElement> path) {
	auto runs = traverse_json(j, path);
	if (!runs || !runs->is_array()) { return ""; }

	std::string result;
	for (const auto &run : *runs) {
		if (run.contains("text") && run["text"].is_string()) {
			result += run["text"].get<std::string>();
		}
	}
	return result;
}

/// Check if a path exists in the JSON
inline bool path_exists(const nlohmann::json &j,
						std::initializer_list<PathElement> path) {
	return detail::traverse(&j, path) != nullptr;
}

}  // namespace ytdlpp::utils
