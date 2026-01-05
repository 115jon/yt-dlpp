#pragma once

#include <boost/outcome.hpp>
#include <system_error>

namespace ytdlpp {

namespace outcome = boost::outcome_v2;

enum class errc {
	success = 0,
	// HTTP/Net errors
	request_failed = 10,
	http_error,	 // Non-200 status

	// Parsing errors
	json_parse_error = 20,
	invalid_url,

	// YouTube specific
	video_not_found = 30,
	extraction_failed,
	decipher_failed,
	n_param_failed,

	// I/O
	file_open_failed = 40,
	file_write_failed,

	// Conversion
	invalid_number_format = 50,

	unknown = 100
};

std::error_code make_error_code(errc e);

}  // namespace ytdlpp

namespace std {
template <>
struct is_error_code_enum<ytdlpp::errc> : true_type {};
}  // namespace std

namespace ytdlpp {
template <typename T>
using Result = outcome::result<T, std::error_code>;
}  // namespace ytdlpp
