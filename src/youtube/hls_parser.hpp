#pragma once

#include <string>
#include <vector>
#include <ytdlpp/types.hpp>

namespace ytdlpp::youtube {

/**
 * @brief Information about an HLS stream variant from the master playlist.
 *
 * Maps to EXT-X-STREAM-INF entries in the M3U8 manifest.
 */
struct HlsStreamInfo {
	std::string url;		  // Full URL to the media playlist
	int bandwidth = 0;		  // BANDWIDTH attribute (bits/sec)
	int width = 0;			  // WIDTH from RESOLUTION
	int height = 0;			  // HEIGHT from RESOLUTION
	double frame_rate = 0.0;  // FRAME-RATE attribute
	std::string codecs;	 // CODECS attribute (e.g., "avc1.640028,mp4a.40.2")
	std::string video_codec;	 // Parsed video codec
	std::string audio_codec;	 // Parsed audio codec
	int itag = 0;				 // YouTube itag from URL (if extractable)
	std::string audio_group_id;	 // AUDIO attribute for audio group
};

/**
 * @brief Parser for HLS (M3U8) manifests.
 *
 * Parses YouTube HLS master playlists to extract available stream variants.
 * Follows the HLS specification (RFC 8216) for EXT-X-STREAM-INF tags.
 */
class HlsParser {
   public:
	/**
	 * @brief Parse an HLS master playlist.
	 *
	 * @param content The M3U8 manifest content
	 * @param base_url Base URL for resolving relative URLs
	 * @return Vector of stream variants found in the manifest
	 */
	static std::vector<HlsStreamInfo> parse_master_playlist(
		const std::string &content, const std::string &base_url);

	/**
	 * @brief Convert HLS stream info to a VideoFormat struct.
	 *
	 * @param stream The HLS stream info to convert
	 * @param video_id Video ID for format_id generation
	 * @return VideoFormat populated with HLS stream data
	 */
	static VideoFormat to_video_format(const HlsStreamInfo &stream,
									   const std::string &video_id);

	/**
	 * @brief Extract YouTube itag from an HLS URL.
	 *
	 * YouTube HLS URLs contain the itag in the path, e.g., /itag/96/
	 *
	 * @param url The HLS stream URL
	 * @return The itag if found, 0 otherwise
	 */
	static int extract_itag(const std::string &url);

   private:
	/**
	 * @brief Parse EXT-X-STREAM-INF attributes.
	 *
	 * @param attrs The attribute string (e.g.,
	 * "BANDWIDTH=123,RESOLUTION=1920x1080")
	 * @return Parsed HlsStreamInfo with attributes filled
	 */
	static HlsStreamInfo parse_stream_inf_attrs(const std::string &attrs);

	/**
	 * @brief Parse codec string into video and audio components.
	 *
	 * @param codecs The CODECS attribute value
	 * @param out_video Output for video codec
	 * @param out_audio Output for audio codec
	 */
	static void parse_codecs(const std::string &codecs, std::string &out_video,
							 std::string &out_audio);

	/**
	 * @brief Resolve a potentially relative URL against a base URL.
	 */
	static std::string resolve_url(const std::string &base,
								   const std::string &url);
};

}  // namespace ytdlpp::youtube
