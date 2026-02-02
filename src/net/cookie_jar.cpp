#include <spdlog/spdlog.h>

#include <algorithm>
#include <string_view>
#include <ytdlpp/cookie_jar.hpp>

namespace ytdlpp::net {

void CookieJar::parse_set_cookies(
	const std::map<std::string, std::string> &headers) {
	// Look for Set-Cookie headers (case-insensitive)
	for (const auto &[name, value] : headers) {
		std::string lower_name = name;
		std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
					   [](unsigned char c) { return std::tolower(c); });
		if (lower_name == "set-cookie") {
			// Value may contain multiple cookies separated by newlines
			// (from HTTP client's multi-header handling)
			std::string_view sv = value;
			size_t pos = 0;
			while (pos < sv.size()) {
				auto nl_pos = sv.find('\n', pos);
				if (nl_pos == std::string_view::npos) {
					// Last cookie
					parse_set_cookie(sv.substr(pos));
					break;
				}
				parse_set_cookie(sv.substr(pos, nl_pos - pos));
				pos = nl_pos + 1;
			}
		}
	}
}

void CookieJar::parse_set_cookie(std::string_view set_cookie_value) {
	// Set-Cookie format: name=value; attribute1; attribute2=value2; ...
	// We only care about the name=value part

	// Find the first semicolon (attributes separator)
	auto semi_pos = set_cookie_value.find(';');
	std::string_view cookie_part =
		(semi_pos != std::string_view::npos)
			? set_cookie_value.substr(0, semi_pos)
			: set_cookie_value;

	// Find the equals sign
	auto eq_pos = cookie_part.find('=');
	if (eq_pos == std::string_view::npos) {
		return;	 // Invalid format
	}

	std::string name(cookie_part.substr(0, eq_pos));
	std::string value(cookie_part.substr(eq_pos + 1));

	// Trim whitespace from name
	while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
		name.erase(0, 1);
	}
	while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
		name.pop_back();
	}

	// Trim whitespace from value
	while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
		value.erase(0, 1);
	}
	while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
		value.pop_back();
	}

	if (!name.empty()) {
		std::lock_guard lock(mutex_);
		cookies_[name] = value;
		spdlog::debug("Cookie stored: {}={}", name,
					  value.substr(0, std::min(size_t{20}, value.size())));
	}
}

void CookieJar::set(const std::string &name, const std::string &value) {
	std::lock_guard lock(mutex_);
	cookies_[name] = value;
}

std::string CookieJar::get(const std::string &name) const {
	std::lock_guard lock(mutex_);
	auto it = cookies_.find(name);
	return (it != cookies_.end()) ? it->second : std::string{};
}

bool CookieJar::has(const std::string &name) const {
	std::lock_guard lock(mutex_);
	return cookies_.find(name) != cookies_.end();
}

std::string CookieJar::build_cookie_header() const {
	std::lock_guard lock(mutex_);
	std::string result;
	for (const auto &[name, value] : cookies_) {
		if (!result.empty()) { result += "; "; }
		result += name + "=" + value;
	}
	return result;
}

void CookieJar::clear() {
	std::lock_guard lock(mutex_);
	cookies_.clear();
}

size_t CookieJar::size() const {
	std::lock_guard lock(mutex_);
	return cookies_.size();
}

}  // namespace ytdlpp::net
