#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "../net/http_client.hpp"

namespace ytdlpp::youtube {

class PlayerScript {
   public:
	explicit PlayerScript(net::HttpClient &http);

	// Downloads the video page, finds the player URL, downloads the player
	// script. Returns the player script content (JS) if successful.
	std::optional<std::string> fetch(const std::string &video_id);

	std::string get_captured_player_url() const { return player_url_; }

   private:
	net::HttpClient &http_;
	std::string player_url_;

	std::optional<std::string> extract_player_url_from_webpage(
		const std::string &webpage);
};

}  // namespace ytdlpp::youtube
