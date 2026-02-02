#pragma once

#include <boost/regex.hpp>
#include <string>
#include <string_view>
#include <ytdlpp/types.hpp>

namespace ytdlpp {

namespace detail {
// Time constants
constexpr int SECONDS_PER_HOUR = 3600;
constexpr int SECONDS_PER_MINUTE = 60;
constexpr int PADDING_THRESHOLD = 10;
constexpr unsigned char MAX_ASCII = 127;
}  // namespace detail

/// Sanitize filename - remove/replace problematic characters
inline std::string sanitize_filename(std::string_view filename,
									 bool restrict_to_ascii = false) {
	std::string result;
	result.reserve(filename.size());

	for (char c : filename) {
		// Characters not allowed in filenames on various OSes
		// Matched to yt-dlp Windows behavior
		switch (c) {
			case '/':
			case '\\': result += '_'; break;
			case ':': result += '#'; break;
			case '*': result += '#'; break;
			case '?': result += '#'; break;
			case '"': result += '\''; break;
			case '<': result += '_'; break;
			case '>': result += '_'; break;
			case '|': result += '#'; break;
			case '\0': result += '_'; break;
			case '\n':
			case '\r':
			case '\t': result += ' '; break;
			default:
				if (restrict_to_ascii &&
					static_cast<unsigned char>(c) > detail::MAX_ASCII) {
					result += '_';
				} else {
					result += c;
				}
				break;
		}
	}

	// Trim trailing spaces and dots (Windows issues)
	while (!result.empty() && (result.back() == ' ' || result.back() == '.')) {
		result.pop_back();
	}

	// Ensure non-empty
	if (result.empty()) { result = "video"; }

	return result;
}

/// Sanitize path - sanitize filename components but preserve directory
/// separators
inline std::string sanitize_path(std::string_view path) {
	std::string result;
	result.reserve(path.size());

	for (char c : path) {
		if (c == '/' || c == '\\') {
			result += c;
			continue;
		}

		switch (c) {
			case ':': result += '#'; break;
			case '*': result += '#'; break;
			case '?': result += '#'; break;
			case '"': result += '\''; break;
			case '<': result += '_'; break;
			case '>': result += '_'; break;
			case '|': result += '#'; break;
			case '\0': result += '_'; break;
			case '\n':
			case '\r':
			case '\t': result += ' '; break;
			default:
				if (static_cast<unsigned char>(c) > detail::MAX_ASCII) {
					// We could sanitize extended ascii if needed, but usually
					// allow
					result += c;
				} else {
					result += c;
				}
				break;
		}
	}
	return result;
}

/// Expand output template using yt-dlp style %(field)s syntax.
/// Supported fields: id, title, ext, uploader, channel, channel_id,
/// upload_date, duration, duration_string, view_count, description,
/// resolution, format, format_id, extractor, extractor_key
inline std::string expand_output_template(
	std::string_view tpl, const VideoInfo &info, std::string_view ext = "",
	bool sanitize_values = true) {
	std::string result(tpl);
	std::string extension = ext.empty() ? info.ext : std::string(ext);

	// Helper to replace field using boost::regex
	auto replace_field = [&result, sanitize_values](
							 const std::string &field, const std::string &value,
							 bool field_sanitize = true) {
		// Match %(field)s or %(field).Ns patterns
		// TODO: Better support for printf-style flags
		boost::regex pattern("%\\(" + field + "\\)(?:[-+0 #]{0,5}\\d{0,5})?s");

		// Only sanitize if global sanitize_values is true AND field_sanitize is
		// true
		std::string replacement =
			(sanitize_values && field_sanitize)
				? sanitize_filename(value)
				: value;
		result = boost::regex_replace(result, pattern, replacement);
	};

	auto replace_int = [&result](const std::string &field, long long value
								 /*, bool sanitize = false*/) {
		// Match integer formatting like %(playlist_index)03d
		// We can support basic padding by using fmt::format or manually parsing
		// For now, simple std::to_string replacement for s/d
		boost::regex pattern_s(
			"%\\(" + field + "\\)(?:[-+0 #]{0,5}\\d{0,5})?s");
		boost::regex pattern_d(
			"%\\(" + field + "\\)(?:[-+0 #]{0,5}\\d{0,5})?d");

		std::string s_val = std::to_string(value);
		result = boost::regex_replace(result, pattern_s, s_val);
		result = boost::regex_replace(result, pattern_d, s_val);
	};

	// Basic fields
	replace_field("id", info.id);
	replace_field("title", info.title);
	replace_field("ext", extension);
	replace_field("uploader", info.uploader);
	replace_field("channel", info.channel);
	replace_field("channel_id", info.channel_id);
	replace_field("upload_date", info.upload_date);
	replace_field("description", info.description);
	replace_field("resolution", info.resolution);
	replace_field("format", info.format);
	replace_field("format_id", info.format_id);

	// Playlist fields
	if (info.playlist_index) {
		replace_int("playlist_index", *info.playlist_index);
	} else {
		replace_field("playlist_index", "NA", false);
	}
	replace_field("playlist", info.playlist_title);
	replace_field("playlist_id", info.playlist_id);

	// Numeric fields
	replace_int("duration", info.duration);
	replace_int("view_count", info.view_count);
	replace_int("like_count", info.like_count);

	// Duration string formatted
	auto dur = info.duration;
	int hours = static_cast<int>(dur / detail::SECONDS_PER_HOUR);
	int minutes = static_cast<int>(
		(dur % detail::SECONDS_PER_HOUR) / detail::SECONDS_PER_MINUTE);
	int seconds = static_cast<int>(dur % detail::SECONDS_PER_MINUTE);
	std::string duration_str;
	if (hours > 0) {
		duration_str =
			std::to_string(hours) + ":" +
			(minutes < detail::PADDING_THRESHOLD ? "0" : "") +
			std::to_string(minutes) + ":" +
			(seconds < detail::PADDING_THRESHOLD ? "0" : "") +
			std::to_string(seconds);
	} else {
		duration_str = std::to_string(minutes) + ":" +
					   (seconds < detail::PADDING_THRESHOLD ? "0" : "") +
					   std::to_string(seconds);
	}
	replace_field("duration_string", duration_str);

	// Extractor fields
	replace_field("extractor", info.extractor);
	replace_field("extractor_key", info.extractor_key);

	return result;
}
}  // namespace ytdlpp
