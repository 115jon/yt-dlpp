#pragma once

#include <string>

namespace ytdlpp::media {

class Muxer {
   public:
	// Merges a video file and an audio file into a single output file.
	// Returns true on success.
	static bool merge(const std::string &video_path,
					  const std::string &audio_path,
					  const std::string &output_path);
};

}  // namespace ytdlpp::media
