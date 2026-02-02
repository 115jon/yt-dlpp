#pragma once

#include <ytdlpp/ytdlpp_export.h>

#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace ytdlpp::net {

/**
 * Simple cookie jar for managing HTTP cookies.
 * Thread-safe for concurrent access.
 *
 * Note: This is a minimal implementation that handles the essential
 * YouTube cookies (PREF, SOCS, YSC, VISITOR_INFO1_LIVE, etc.).
 * It does not implement full RFC 6265 compliance (expiry, path, domain
 * matching, etc.) as that's overkill for our use case.
 */
class YTDLPP_EXPORT CookieJar {
   public:
	CookieJar() = default;

	/**
	 * Parse and store cookies from Set-Cookie headers.
	 * @param headers Response headers (may contain multiple Set-Cookie entries)
	 */
	void parse_set_cookies(const std::map<std::string, std::string> &headers);

	/**
	 * Parse a single Set-Cookie header value and store the cookie.
	 * @param set_cookie_value The value of a Set-Cookie header
	 */
	void parse_set_cookie(std::string_view set_cookie_value);

	/**
	 * Set a cookie manually.
	 * @param name Cookie name
	 * @param value Cookie value
	 */
	void set(const std::string &name, const std::string &value);

	/**
	 * Get a cookie value.
	 * @param name Cookie name
	 * @return Cookie value, or empty string if not found
	 */
	std::string get(const std::string &name) const;

	/**
	 * Check if a cookie exists.
	 * @param name Cookie name
	 * @return true if the cookie exists
	 */
	bool has(const std::string &name) const;

	/**
	 * Build the Cookie header value for an outgoing request.
	 * @return Cookie header value (e.g., "name1=value1; name2=value2")
	 */
	std::string build_cookie_header() const;

	/**
	 * Clear all cookies.
	 */
	void clear();

	/**
	 * Get the number of stored cookies.
	 */
	size_t size() const;

   private:
	mutable std::mutex mutex_;
	std::map<std::string, std::string> cookies_;
};

}  // namespace ytdlpp::net
