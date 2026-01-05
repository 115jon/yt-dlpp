#include "error.hpp"

#include <string>

namespace ytdlpp {

struct ytdlpp_error_category : std::error_category {
	const char *name() const noexcept override { return "ytdlpp"; }

	std::string message(int ev) const override {
		switch (static_cast<errc>(ev)) {
			case errc::success: return "Success";
			case errc::request_failed: return "Request failed";
			case errc::http_error: return "HTTP error";
			case errc::json_parse_error: return "JSON parse error";
			case errc::invalid_url: return "Invalid URL";
			case errc::video_not_found: return "Video not found";
			case errc::extraction_failed: return "Extraction failed";
			case errc::decipher_failed: return "Decipher failed";
			case errc::n_param_failed:
				return "N-parameter transformation failed";
			case errc::file_open_failed: return "File open failed";
			case errc::file_write_failed: return "File write failed";
			case errc::invalid_number_format: return "Invalid number format";
			default: return "Unknown error";
		}
	}
};

const std::error_category &ytdlpp_category() {
	static ytdlpp_error_category category;
	return category;
}

std::error_code make_error_code(errc e) {
	return {static_cast<int>(e), ytdlpp_category()};
}

}  // namespace ytdlpp
