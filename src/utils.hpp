#pragma once

#include <boost/charconv.hpp>
#include <charconv>
#include <string>
#include <string_view>
#include <ytdlpp/result.hpp>

namespace ytdlpp::utils {

// Safe conversions utilizing boost::charconv
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

}  // namespace ytdlpp::utils
