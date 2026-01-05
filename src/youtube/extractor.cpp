#include "extractor.hpp"

#include <spdlog/spdlog.h>

#include <boost/url.hpp>

#include "decipher.hpp"
#include "player_script.hpp"
#include "utils.hpp"

namespace ytdlpp::youtube {

Extractor::Extractor(net::HttpClient &http, scripting::JsEngine &js)
	: http_(http), js_(js) {}

Result<VideoInfo> Extractor::process(const std::string &url) {
	std::string video_id = extract_video_id(url);
	if (video_id.empty()) {
		spdlog::error("Invalid YouTube URL");
		return outcome::failure(errc::invalid_url);
	}

	spdlog::info("Extracting URL: {}", url);

	// 1. Fetch Player Script
	spdlog::info("{}: Downloading webpage", video_id);

	PlayerScript player_script(http_);
	auto script_content = player_script.fetch(video_id);

	// We pass the JS engine to the key decipherer
	SigDecipherer decipherer(js_);
	if (script_content) {
		spdlog::info("[jsc:quickjs] Solving JS challenges using quickjs");
		if (decipherer.load_functions(*script_content)) {
			// success
		} else {
			spdlog::warn(
				"Failed to load decipher functions. Downloads may fail.");
		}
	} else {
		spdlog::warn(
			"Could not download player script. Signature deciphering "
			"unavailable.");
	}

	// 2. Fallback strategy: Android -> iOS -> Web
	std::vector<InnertubeContext> clients = {
		Innertube::CLIENT_WEB, Innertube::CLIENT_ANDROID,
		Innertube::CLIENT_IOS};

	// We'll store responses with their client names to correlate warnings
	std::vector<std::pair<std::string, nlohmann::json>> responses_with_clients;

	for (const auto &client : clients) {
		std::string friendly_name;
		if (client.client_name == "WEB")
			friendly_name = "web";
		else if (client.client_name == "ANDROID")
			friendly_name = "android";
		else if (client.client_name == "IOS")
			friendly_name = "ios";
		else
			friendly_name = client.client_name;

		spdlog::info(
			"{}: Downloading {} player API JSON", video_id, friendly_name);

		auto info = get_info_with_client(video_id, client);
		if (info.has_value()) {
			responses_with_clients.push_back(
				{client.client_name, info.value()});
		}
	}

	if (responses_with_clients.empty()) {
		spdlog::error("All clients failed to get video info.");
		return outcome::failure(errc::video_not_found);
	}

	// Process formats
	VideoInfo info;
	info.id = video_id;
	info.webpage_url = "https://www.youtube.com/watch?v=" + video_id;

	const nlohmann::json &json =
		responses_with_clients[0].second;  // Primary metadata source

	if (json.contains("videoDetails")) {
		const auto &details = json["videoDetails"];
		info.title = details.value("title", "");
		info.description = details.value("shortDescription", "");
		info.uploader = details.value("author", "");
		info.uploader_id = details.value("channelId", "");
		info.duration = utils::to_number_default<long long>(
			details.value("lengthSeconds", "0"));
		info.view_count = utils::to_number_default<long long>(
			details.value("viewCount", "0"));

		if (details.contains("thumbnail") &&
			details["thumbnail"].contains("thumbnails")) {
			const auto &thumbs = details["thumbnail"]["thumbnails"];
			if (!thumbs.empty()) {
				info.thumbnail = thumbs.back().value("url", "");
			}
		}
	}

	// Microformat for upload date
	if (json.contains("microformat") &&
		json["microformat"].contains("playerMicroformatRenderer")) {
		auto upload_date_raw =
			json["microformat"]["playerMicroformatRenderer"].value(
				"uploadDate", "");
		// Format is usually YYYY-MM-DD, yt-dlp expects YYYYMMDD
		if (!upload_date_raw.empty()) {
			upload_date_raw.erase(std::remove(upload_date_raw.begin(),
											  upload_date_raw.end(), '-'),
								  upload_date_raw.end());
			info.upload_date = upload_date_raw;
		}
	}

	for (const auto &[client_name, resp] : responses_with_clients) {
		bool skipped_any_due_to_url = false;

		auto process_fmt = [&](const nlohmann::json &fmt_json) {
			VideoFormat fmt{};
			fmt.itag = fmt_json.value("itag", 0);
			fmt.url = fmt_json.value("url", "");
			fmt.mime_type = fmt_json.value("mimeType", "");
			fmt.width = fmt_json.value("width", 0);
			fmt.height = fmt_json.value("height", 0);
			fmt.fps = fmt_json.value("fps", 0);
			fmt.audio_sample_rate = utils::to_number_default<int>(
				fmt_json.value("audioSampleRate", "0"));
			fmt.audio_channels = fmt_json.value("audioChannels", 0);

			// Bitrates
			if (fmt_json.contains("bitrate"))
				fmt.tbr = fmt_json["bitrate"].get<double>() / 1000.0;
			if (fmt_json.contains("averageBitrate"))
				fmt.tbr = fmt_json["averageBitrate"].get<double>() / 1000.0;
			if (fmt_json.contains("contentLength")) {
				fmt.content_length = utils::to_number_default<long long>(
					fmt_json.value("contentLength", "0"));
			}

			// Codec parsing
			if (!fmt.mime_type.empty()) {
				auto semi = fmt.mime_type.find(';');
				std::string type_part = fmt.mime_type.substr(0, semi);

				auto slash = type_part.find('/');
				if (slash != std::string::npos) {
					std::string main_type = type_part.substr(0, slash);
					std::string sub_type = type_part.substr(slash + 1);
					if (main_type == "audio" && sub_type == "mp4")
						fmt.ext = "m4a";
					else if (main_type == "audio" && sub_type == "webm")
						fmt.ext = "webm";
					else
						fmt.ext = sub_type;
				}

				auto codecs_pos = fmt.mime_type.find("codecs=\"");
				if (codecs_pos != std::string::npos) {
					auto start = codecs_pos + 8;
					auto end = fmt.mime_type.find('\"', start);
					if (end != std::string::npos) {
						std::string codecs =
							fmt.mime_type.substr(start, end - start);
						auto comma = codecs.find(',');
						if (comma != std::string::npos) {
							fmt.vcodec = codecs.substr(0, comma);
							fmt.acodec = codecs.substr(comma + 2);
						} else {
							if (type_part.find("audio") == 0) {
								fmt.vcodec = "none";
								fmt.acodec = codecs;
							} else {
								fmt.vcodec = codecs;
								fmt.acodec = "none";
							}
						}
					}
				} else {
					fmt.vcodec = "none";
					fmt.acodec = "none";
				}
			}

			// Deciphering logic if URL is missing
			if (fmt.url.empty() && fmt_json.contains("signatureCipher")) {
				std::string cipher = fmt_json["signatureCipher"];
				try {
					// Manual parsing
					std::string s, sp, url_raw;

					auto decode_url = [](const std::string &in) -> std::string {
						std::string out;
						out.reserve(in.size());
						for (size_t i = 0; i < in.size(); ++i) {
							if (in[i] == '%') {
								if (i + 2 < in.size()) {
									int value = 0;
									std::istringstream is(in.substr(i + 1, 2));
									if (is >> std::hex >> value) {
										out += static_cast<char>(value);
										i += 2;
									} else {
										out += in[i];
									}
								} else {
									out += in[i];
								}
							} else if (in[i] == '+') {
								out += ' ';
							} else {
								out += in[i];
							}
						}
						return out;
					};

					size_t pos = 0;
					while (pos < cipher.length()) {
						size_t amp = cipher.find('&', pos);
						if (amp == std::string::npos) amp = cipher.length();

						std::string pair = cipher.substr(pos, amp - pos);
						size_t eq = pair.find('=');
						if (eq != std::string::npos) {
							std::string key = pair.substr(0, eq);
							std::string val = decode_url(pair.substr(eq + 1));

							if (key == "s")
								s = val;
							else if (key == "sp")
								sp = val;
							else if (key == "url")
								url_raw = val;
						}
						pos = amp + 1;
					}

					if (!url_raw.empty() && !s.empty()) {
						std::string sig = decipherer.decipher_signature(s);
						if (url_raw.find('?') == std::string::npos)
							url_raw += "?";
						else
							url_raw += "&";
						url_raw += (sp.empty() ? "sig" : sp) + "=" + sig;

						// N-param
						boost::urls::url url_obj(url_raw);
						std::string n_val;
						// Manual search
						for (auto p : url_obj.params()) {
							if (p.key == "n") {
								n_val = p.value;
								break;
							}
						}

						if (!n_val.empty()) {
							std::string new_n = decipherer.transform_n(n_val);
							boost::urls::params_ref params = url_obj.params();
							auto it = params.find("n");
							if (it != params.end())
								params.replace(it, {"n", new_n});
							fmt.url = std::string(url_obj.buffer().data(),
												  url_obj.buffer().size());
						} else {
							fmt.url = url_raw;
						}
					}
				} catch (...) {}
			} else if (!fmt.url.empty()) {
				// Handle N-param on direct URLs too
				try {
					boost::urls::url url_obj(fmt.url);
					std::string n_val;
					for (auto p : url_obj.params()) {
						if (p.key == "n") {
							n_val = p.value;
							break;
						}
					}
					if (!n_val.empty()) {
						std::string new_n = decipherer.transform_n(n_val);
						spdlog::debug(
							"N-param transformation: {} -> {}", n_val, new_n);
						boost::urls::params_ref params = url_obj.params();
						auto it = params.find("n");
						if (it != params.end())
							params.replace(it, {"n", new_n});
						fmt.url = std::string(
							url_obj.buffer().data(), url_obj.buffer().size());
					}
				} catch (...) {}
			}

			if (fmt.url.empty()) {
				spdlog::debug(
					"Skipping format {} because URL is empty", fmt.itag);
				skipped_any_due_to_url = true;
				return;
			}

			info.formats.push_back(fmt);
		};

		if (resp.contains("streamingData")) {
			if (resp["streamingData"].contains("formats")) {
				for (const auto &f : resp["streamingData"]["formats"])
					process_fmt(f);
			}
			if (resp["streamingData"].contains("adaptiveFormats")) {
				for (const auto &f : resp["streamingData"]["adaptiveFormats"])
					process_fmt(f);
			}
		}

		if (skipped_any_due_to_url) {
			std::string msg;
			if (client_name == "WEB" || client_name == "web") {
				msg = fmt::format(
					"WARNING: {}: Some web client https formats have been "
					"skipped as they are missing a url. "
					"YouTube is forcing SABR streaming for this client. "
					"See https://github.com/yt-dlp/yt-dlp/issues/12482 for "
					"more details",
					video_id);
			} else if (client_name == "ANDROID" || client_name == "android" ||
					   client_name == "tv") {
				msg = fmt::format(
					"WARNING: {}: Some tv client https formats have been "
					"skipped as they are missing a url. "
					"YouTube may have enabled the SABR-only or Server-Side Ad "
					"Placement experiment for the current session. "
					"See https://github.com/yt-dlp/yt-dlp/issues/12482 for "
					"more details",
					video_id);
			} else {
				msg = fmt::format(
					"WARNING: {}: Some {} client https formats have been "
					"skipped as they are missing a url.",
					video_id, client_name);
			}
			spdlog::warn(msg);
		}
	}

	return info;
}

Result<nlohmann::json> Extractor::get_info_with_client(
	const std::string &video_id, const InnertubeContext &client) {
	std::string api_url = "https://www.youtube.com/youtubei/v1/player";

	nlohmann::json payload = Innertube::build_context(client);
	payload["videoId"] = video_id;
	payload["contentCheckOk"] = true;
	payload["racyCheckOk"] = true;

	auto headers = Innertube::get_headers(client);

	auto res = http_.post(api_url, payload.dump(), headers);
	if (res.has_error()) {
		spdlog::warn(
			"Client {} failed: {}", client.client_name, res.error().message());
		return outcome::failure(res.error());
	}

	auto r = res.value();
	if (r.status_code != 200) {
		spdlog::warn("Client {} failed with status {}", client.client_name,
					 r.status_code);
		return outcome::failure(errc::request_failed);
	}

	try {
		auto json = nlohmann::json::parse(r.body);
		if (json.contains("playabilityStatus") &&
			json["playabilityStatus"]["status"] != "OK") {
			spdlog::warn(
				"Video unplayable with client {}: {}", client.client_name,
				json["playabilityStatus"]["status"].get<std::string>());
			return outcome::failure(errc::video_not_found);
		}
		return json;
	} catch (...) { return outcome::failure(errc::json_parse_error); }
}

std::string Extractor::extract_video_id(const std::string &url_str) {
	try {
		auto u_res = boost::urls::parse_uri(url_str);
		if (u_res.has_error()) return "";
		auto u = u_res.value();

		if (u.host().find("youtu.be") != std::string::npos) {
			std::string path = u.path();
			if (!path.empty() && path[0] == '/') path = path.substr(1);
			return path;
		}
		if (u.params().contains("v")) {
			auto it = u.params().find("v");
			return (*it).value;
		}
	} catch (...) {}
	return "";
}

void to_json(nlohmann::json &j, const VideoFormat &f) {
	j = nlohmann::json{
		{"format_id", std::to_string(f.itag)},
		{"url", f.url},
		{"filesize", f.content_length},
		{"vcodec", f.vcodec},
		{"acodec", f.acodec},
		{"ext", f.ext},
		{"fps", f.fps},
		{"asr", f.audio_sample_rate},
		{"audio_channels", f.audio_channels},
		{"tbr", f.tbr}};

	// Explicit null handling
	if (f.width > 0)
		j["width"] = f.width;
	else
		j["width"] = nullptr;
	if (f.height > 0)
		j["height"] = f.height;
	else
		j["height"] = nullptr;

	// Derived bitrates
	if (f.tbr > 0) {
		if (f.vcodec == "none" && f.acodec != "none") {
			j["abr"] = f.tbr;
			j["vbr"] = 0;
		} else if (f.acodec == "none" && f.vcodec != "none") {
			j["vbr"] = f.tbr;
			j["abr"] = 0;
		}
	}
}

void to_json(nlohmann::json &j, const VideoInfo &i) {
	j = nlohmann::json{
		{"id", i.id},
		{"title", i.title},
		{"description", i.description},
		{"uploader", i.uploader},
		{"uploader_id", i.uploader_id},
		{"upload_date", i.upload_date},
		{"duration", i.duration},
		{"view_count", i.view_count},
		{"webpage_url", i.webpage_url},
		{"thumbnail", i.thumbnail},
		{"formats", i.formats}};
}

}  // namespace ytdlpp::youtube
