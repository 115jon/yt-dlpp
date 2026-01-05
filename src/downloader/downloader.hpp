#pragma once

#include <optional>
#include <string>

#include "net/http_client.hpp"
#include "youtube/extractor.hpp"

namespace ytdlpp {

class Downloader {
   public:
	explicit Downloader(net::HttpClient &http);

	bool download(
		const youtube::VideoInfo &info,
		const std::string &format_selector = "best",
		const std::optional<std::string> &merge_format = std::nullopt);

	struct StreamInfo {
		const youtube::VideoFormat *video = nullptr;
		const youtube::VideoFormat *audio = nullptr;
	};

	StreamInfo select_streams(const youtube::VideoInfo &info,
							  const std::string &selector);

   private:
	net::HttpClient &http_;

	bool download_stream(const youtube::VideoFormat &format,
						 const std::string &output_path);
	bool merge_streams(const std::string &video_path,
					   const std::string &audio_path,
					   const std::string &output_path);
	std::string sanitize_filename(const std::string &name);
};

}  // namespace ytdlpp
