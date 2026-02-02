#include "hls_parser.hpp"

#include <spdlog/spdlog.h>

#include <boost/regex.hpp>
#include <boost/url.hpp>
#include <sstream>

namespace ytdlpp::youtube {

namespace {

// Trim whitespace from both ends
std::string trim(std::string_view sv) {
	while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front())))
		sv.remove_prefix(1);
	while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back())))
		sv.remove_suffix(1);
	return std::string(sv);
}

}  // namespace

int HlsParser::extract_itag(const std::string &url) {
	// YouTube HLS URLs contain /itag/NUMBER/ in the path
	// This is used to identify the format from the URL
	try {
		static const boost::regex re(R"(/itag/(\d+)/)");
		boost::smatch match;
		if (boost::regex_search(url, match, re)) {
			return std::stoi(match[1].str());
		}
	} catch (...) {}
	return 0;
}

void HlsParser::parse_codecs(const std::string &codecs, std::string &out_video,
							 std::string &out_audio) {
	// Codec string format: "avc1.640028,mp4a.40.2"
	// Video codecs start with: avc1, vp9, av01, hev1, hvc1
	// Audio codecs start with: mp4a, opus, ac-3, ec-3

	std::istringstream iss(codecs);
	std::string codec;
	while (std::getline(iss, codec, ',')) {
		codec = trim(codec);
		if (codec.empty()) continue;

		// Check for video codec prefixes
		if (codec.find("avc1") == 0 || codec.find("vp9") == 0 ||
			codec.find("vp09") == 0 || codec.find("av01") == 0 ||
			codec.find("hev1") == 0 || codec.find("hvc1") == 0) {
			if (out_video.empty()) { out_video = codec; }
		}
		// Check for audio codec prefixes
		else if (codec.find("mp4a") == 0 || codec.find("opus") == 0 ||
				 codec.find("ac-3") == 0 || codec.find("ec-3") == 0) {
			if (out_audio.empty()) { out_audio = codec; }
		}
	}
}

HlsStreamInfo HlsParser::parse_stream_inf_attrs(const std::string &attrs) {
	HlsStreamInfo info;

	// Use regex to extract key-value pairs - more reliable than manual parsing
	try {
		// Extract BANDWIDTH
		static const boost::regex re_bw(R"(BANDWIDTH=(\d+))");
		boost::smatch m;
		if (boost::regex_search(attrs, m, re_bw)) {
			info.bandwidth = std::stoi(m[1].str());
		}

		// Extract RESOLUTION
		static const boost::regex re_res(R"(RESOLUTION=(\d+)x(\d+))");
		if (boost::regex_search(attrs, m, re_res)) {
			info.width = std::stoi(m[1].str());
			info.height = std::stoi(m[2].str());
		}

		// Extract FRAME-RATE
		static const boost::regex re_fps(R"RE(FRAME-RATE=([\d.]+))RE");
		if (boost::regex_search(attrs, m, re_fps)) {
			info.frame_rate = std::stod(m[1].str());
		}

		// Extract CODECS (quoted)
		static const boost::regex re_codecs(R"RE(CODECS="([^"]+)")RE");
		if (boost::regex_search(attrs, m, re_codecs)) {
			info.codecs = m[1].str();
			parse_codecs(info.codecs, info.video_codec, info.audio_codec);
		}

		// Extract AUDIO group
		static const boost::regex re_audio(R"RE(AUDIO="([^"]+)")RE");
		if (boost::regex_search(attrs, m, re_audio)) {
			info.audio_group_id = m[1].str();
		}
	} catch (const std::exception &e) {
		spdlog::warn("Error parsing HLS attributes: {}", e.what());
	}

	spdlog::debug(
		"HLS parsed: bw={}, res={}x{}, fps={:.0f}, codecs={}", info.bandwidth,
		info.width, info.height, info.frame_rate, info.codecs);

	return info;
}

std::string HlsParser::resolve_url(const std::string &base,
								   const std::string &url) {
	// If already absolute, return as-is
	if (url.find("http://") == 0 || url.find("https://") == 0) { return url; }

	try {
		auto base_result = boost::urls::parse_uri(base);
		if (!base_result) return url;

		boost::urls::url resolved(*base_result);

		if (url.empty()) return base;

		if (url[0] == '/') {
			// Absolute path
			resolved.set_path(url);
		} else {
			// Relative path - append to base path
			std::string base_path = resolved.path();
			auto last_slash = base_path.rfind('/');
			if (last_slash != std::string::npos) {
				resolved.set_path(base_path.substr(0, last_slash + 1) + url);
			} else {
				resolved.set_path("/" + url);
			}
		}

		return std::string(resolved.buffer());
	} catch (...) { return url; }
}

std::vector<HlsStreamInfo> HlsParser::parse_master_playlist(
	const std::string &content, const std::string &base_url) {
	std::vector<HlsStreamInfo> streams;

	// Log first part of content for debugging
	spdlog::debug("HLS manifest content (first 500 chars): {}",
				  content.substr(0, std::min(size_t{500}, content.size())));

	std::istringstream iss(content);
	std::string line;
	HlsStreamInfo pending_info;
	bool has_pending = false;

	while (std::getline(iss, line)) {
		// Remove carriage return if present (Windows line endings)
		if (!line.empty() && line.back() == '\r') { line.pop_back(); }

		// Skip empty lines
		if (line.empty()) continue;

		// Check for EXT-X-STREAM-INF tag
		if (line.find("#EXT-X-STREAM-INF:") == 0) {
			// Parse attributes after the tag
			std::string attrs =
				line.substr(18);  // Length of "#EXT-X-STREAM-INF:"
			spdlog::debug("Found EXT-X-STREAM-INF: {}", attrs);
			pending_info = parse_stream_inf_attrs(attrs);
			has_pending = true;
		}
		// Regular line after EXT-X-STREAM-INF is the URL
		else if (has_pending && line[0] != '#') {
			pending_info.url = resolve_url(base_url, line);
			pending_info.itag = extract_itag(pending_info.url);
			spdlog::debug("Stream URL: {} (itag={}, bw={}, {}x{})",
						  pending_info.url.substr(0, 60), pending_info.itag,
						  pending_info.bandwidth, pending_info.width,
						  pending_info.height);

			// Only add if we have meaningful data
			if (!pending_info.url.empty() &&
				(pending_info.bandwidth > 0 || pending_info.height > 0)) {
				streams.push_back(pending_info);
			}

			has_pending = false;
			pending_info = HlsStreamInfo{};
		}
		// Any other tag resets pending state
		else if (line[0] == '#') {
			// Don't reset for other tags - some manifests have extra tags
			// between EXT-X-STREAM-INF and URL
		}
	}

	spdlog::debug(
		"Parsed {} HLS stream variants from manifest", streams.size());
	return streams;
}

VideoFormat HlsParser::to_video_format(const HlsStreamInfo &stream,
									   const std::string &video_id) {
	VideoFormat fmt;

	// Set itag from stream
	fmt.itag = stream.itag;

	// Generate format ID from itag or resolution
	if (stream.itag > 0) {
		fmt.format_id = std::to_string(stream.itag);
	} else {
		fmt.format_id = "hls-" + std::to_string(stream.height) + "p";
	}

	// Set URL - use the HLS playlist URL directly from the master manifest
	// This URL contains all the authentication and signature parameters needed
	fmt.url = stream.url;

	// Resolution
	fmt.width = stream.width;
	fmt.height = stream.height;

	// Frame rate (default to 30 if not specified)
	fmt.fps = stream.frame_rate > 0 ? stream.frame_rate : 30.0;

	// Codecs
	fmt.vcodec = stream.video_codec.empty() ? "none" : stream.video_codec;
	fmt.acodec = stream.audio_codec.empty() ? "none" : stream.audio_codec;

	// HLS is always mp4 container for YouTube
	fmt.ext = "mp4";

	// Bitrate
	fmt.tbr =
		static_cast<double>(stream.bandwidth) / 1000.0;	 // Convert to kbps

	// Protocol
	fmt.protocol = "m3u8";

	// Audio channels (YouTube HLS is typically stereo)
	fmt.audio_channels = 2;

	// Format note
	std::ostringstream note;
	note << stream.height << "p";
	if (stream.frame_rate > 30) { note << static_cast<int>(stream.frame_rate); }
	fmt.format_note = note.str();

	// Set source_preference based on resolution and protocol
	// HLS formats should have lower preference than direct formats (-1)
	fmt.source_preference = -10;  // Default for HLS

	// Adjust based on resolution (higher resolution = higher preference)
	if (stream.height >= 1080) {
		fmt.source_preference += 2;
	} else if (stream.height >= 720) {
		fmt.source_preference += 1;
	}

	return fmt;
}

}  // namespace ytdlpp::youtube
