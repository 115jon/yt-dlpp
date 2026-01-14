#include <spdlog/spdlog.h>

#include <boost/regex.hpp>
#include <boost/url.hpp>
#include <fstream>
#include <mutex>
#include <set>
#include <ytdlpp/ejs_solver.hpp>
#include <ytdlpp/extractor.hpp>

#include "decipher.hpp"
#include "innertube.hpp"
#include "player_script.hpp"
#include "scripting/js_engine.hpp"
#include "utils.hpp"

namespace ytdlpp::youtube {

// Helper to extract video ID
static std::string extract_video_id(const std::string &url_str) {
	try {
		// Simple regex to catch various ID formats
		// Matches:
		// - youtube.com/watch?v=ID
		// - youtube.com/shorts/ID
		// - youtube.com/embed/ID
		// - youtube.com/v/ID
		// - youtu.be/ID
		static const boost::regex re(
			R"(^(?:https?://)?(?:www\.|m\.)?(?:youtube\.com/(?:watch\?v=|shorts/|embed/|v/)|youtu\.be/)([\w-]{11}))");

		boost::smatch m;
		if (boost::regex_search(url_str, m, re)) { return m[1]; }
	} catch (...) {}
	return "";
}

using InfoHandler = asio::any_completion_handler<void(Result<VideoInfo>)>;

// Internal Session managing the process
struct AsyncSession : public std::enable_shared_from_this<AsyncSession> {
	using CompletionExecutor = Extractor::CompletionExecutor;

	std::shared_ptr<net::HttpClient> http;
	std::shared_ptr<scripting::JsEngine> js;
	std::string url;
	InfoHandler handler;
	CompletionExecutor handler_ex;
	std::string video_id;

	PlayerScript player_script;
	SigDecipherer decipherer;
	std::vector<std::pair<std::string, nlohmann::json>> responses;
	std::atomic<bool> cancelled{false};
	VideoInfo collected_info;  // Store info being built

	static const std::vector<InnertubeContext> &get_clients();

	AsyncSession(std::shared_ptr<net::HttpClient> h,
				 std::shared_ptr<scripting::JsEngine> j, std::string u,
				 InfoHandler handler, CompletionExecutor handler_ex)
		: http(std::move(h)),
		  js(std::move(j)),
		  url(std::move(u)),
		  handler(std::move(handler)),
		  handler_ex(std::move(handler_ex)),
		  player_script(*http),
		  decipherer(*js) {}

	void cancel() { cancelled = true; }

	void complete(Result<VideoInfo> result) {
		asio::dispatch(handler_ex, [h = std::move(handler),
									result = std::move(result)]() mutable {
			h(std::move(result));
		});
	}

	void start() {
		if (cancelled) return;
		video_id = extract_video_id(url);
		if (video_id.empty()) {
			spdlog::error("Invalid YouTube URL");
			return complete(outcome::failure(errc::invalid_url));
		}

		spdlog::info("{}: Downloading webpage", video_id);

		auto self = shared_from_this();
		player_script.async_fetch(
			video_id,
			[self](std::optional<std::string> content) {
				if (self->cancelled) return;
				self->on_script(std::move(content));
			},
			[self](const std::string &webpage) {
				if (self->cancelled) return;
				self->extract_web_tokens(webpage);
			});
	}

	void on_script(std::optional<std::string> content) {
		if (cancelled) return;
		if (content) {
			auto self = shared_from_this();
			std::string player_id;
			std::string player_url =
				self->player_script.get_captured_player_url();
			boost::regex re("/player/([a-zA-Z0-9]+)/");
			boost::smatch match;
			if (boost::regex_search(player_url, match, re)) {
				if (match.size() > 1) { player_id = match.str(1); }
			}
			self->decipherer.async_load_functions(
				*content,
				[self](bool success) {
					if (self->cancelled) return;
					if (!success) {
						spdlog::debug(
							"Failed to load decipher functions. Downloads may "
							"fail.");
					}
					// Fetch TV config page first, then fetch all clients
					self->fetch_tv_config();
				},
				player_id);
		} else {
			spdlog::debug(
				"Could not download player script. Signature deciphering "
				"unavailable.");
			// Fetch TV config page first, then fetch all clients
			fetch_tv_config();
		}
	}

	// Extract visitor data from ytcfg.set({...}) in HTML
	std::string extract_visitor_data(const std::string &html) {
		// Look for ytcfg.set({...})
		std::string search = "ytcfg.set(";
		size_t pos = html.find(search);
		if (pos == std::string::npos) return "";

		pos += search.length();
		if (pos >= html.length() || html[pos] != '{') return "";

		// Find matching closing brace
		int brace_count = 1;
		size_t start = pos;
		pos++;
		while (pos < html.length() && brace_count > 0) {
			if (html[pos] == '{')
				brace_count++;
			else if (html[pos] == '}')
				brace_count--;
			pos++;
		}

		if (brace_count != 0) return "";

		std::string json_str = html.substr(start, pos - start);
		// Parse without throwing
		auto ytcfg = nlohmann::json::parse(json_str, nullptr, false);
		if (ytcfg.is_discarded()) return "";

		// Try multiple paths for VISITOR_DATA
		if (ytcfg.contains("VISITOR_DATA")) {
			return ytcfg["VISITOR_DATA"].get<std::string>();
		}
		if (ytcfg.contains("INNERTUBE_CONTEXT") &&
			ytcfg["INNERTUBE_CONTEXT"].contains("client") &&
			ytcfg["INNERTUBE_CONTEXT"]["client"].contains("visitorData")) {
			return ytcfg["INNERTUBE_CONTEXT"]["client"]["visitorData"]
				.get<std::string>();
		}

		return "";
	}

	// Fetch TV config page to get visitor data
	void fetch_tv_config() {
		if (cancelled) return;

		auto self = shared_from_this();
		std::string tv_url = "https://www.youtube.com/tv";

		spdlog::info("{}: Downloading tv client config", video_id);

		std::map<std::string, std::string> headers = {
			{"User-Agent",
			 "Mozilla/5.0 (ChromiumStylePlatform) Cobalt/Version"},
			{"Accept", "text/html"},
		};

		http->async_get(
			tv_url,
			[self](Result<net::HttpResponse> res) {
				if (self->cancelled) return;

				if (res.has_value() && res.value().status_code == 200) {
					self->tv_visitor_data_ = self->extract_visitor_data(
						res.value()
							.body);	 // Keep TV extraction as is or merge logic?
					// Actually tv_visitor_data is specific to TV endpoint?
					// "X-Goog-Visitor-Id" header for TV client.
					// We will keep it separate.
					if (!self->tv_visitor_data_.empty()) {
						spdlog::debug("Got TV visitor data: {}...",
									  self->tv_visitor_data_.substr(0, 20));
					}
				}

				// Proceed to fetch all clients
				self->fetch_all_clients();
			},
			headers);
	}

	// Store TV visitor data
	std::string tv_visitor_data_;
	// Store Web visitor data and PO Token
	std::string web_visitor_data_;
	std::string po_token_;

	// Parse format metadata fields (sync part)
	VideoFormat parse_format_metadata(const nlohmann::json &fmt_json) {
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

		if (fmt_json.contains("bitrate"))
			fmt.tbr = fmt_json["bitrate"].get<double>() / 1000.0;
		if (fmt_json.contains("averageBitrate"))
			fmt.tbr = fmt_json["averageBitrate"].get<double>() / 1000.0;
		if (fmt_json.contains("contentLength")) {
			fmt.content_length = utils::to_number_default<long long>(
				fmt_json.value("contentLength", "0"));
		}

		if (fmt_json.contains("audioTrack")) {
			const auto &at = fmt_json["audioTrack"];
			std::string display_name = at.value("displayName", "");
			std::string id = at.value("id", "");
			bool is_default = at.value("audioIsDefault", false);

			if (!id.empty()) {
				size_t dot = id.find('.');
				if (dot != std::string::npos)
					fmt.language = id.substr(0, dot);
				else
					fmt.language = id;
			}

			std::string dn_lower = display_name;
			std::transform(dn_lower.begin(), dn_lower.end(), dn_lower.begin(),
						   [](unsigned char c) { return std::tolower(c); });

			if (dn_lower.find("descriptive") != std::string::npos) {
				if (!fmt.language.empty()) fmt.language += "-desc";
				fmt.language_preference = -10;
			} else if (dn_lower.find("original") != std::string::npos) {
				fmt.language_preference = 10;
			} else if (is_default) {
				fmt.language_preference = 5;
			} else {
				fmt.language_preference = -1;
			}
		}

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
		return fmt;
	}

	void async_process_url_n(const std::string &url_raw,
							 std::function<void(std::string)> cb) {
		boost::system::result<boost::urls::url> r =
			boost::urls::parse_uri(url_raw);
		if (r.has_value()) {
			boost::urls::url url_obj = *r;
			std::string n_val;
			for (auto p : url_obj.params()) {
				if (p.key == "n") {
					n_val = p.value;
					break;
				}
			}
			if (!n_val.empty()) {
				decipherer.async_transform_n(
					n_val, [this, url_obj, cb](std::string new_n) mutable {
						boost::urls::params_ref params = url_obj.params();
						auto it = params.find("n");
						if (it != params.end())
							params.replace(it, {"n", new_n});
						cb(std::string(
							url_obj.buffer().data(), url_obj.buffer().size()));
					});
				return;
			}
		}
		cb(url_raw);
	}

	void extract_web_tokens(const std::string &html) {
		// reuse extract_visitor_data logic but store into web_visitor_data_
		// Also look for poToken

		// 1. Visitor Data from ytcfg
		web_visitor_data_ = extract_visitor_data(html);
		if (!web_visitor_data_.empty()) {
			spdlog::debug("Extracted WEB visitor data: {}...",
						  web_visitor_data_.substr(0, 20));
		}

		// 2. PO Token
		// yt-dlp reference: relies on external 'bg_utils' provider or cached
		// WebPO. Core yt-dlp does not scrape this from HTML by default, but we
		// attempt to do so to support "WebPO" strategy if the token is present
		// in ytcfg. It can be in ytcfg.set({ "INNERTUBE_CONTEXT": { ...,
		// "serviceIntegrityDimensions": { "poToken": "..." } } }) Regex for
		// "poToken":"..." allowing single/double quotes
		try {
			static const boost::regex re_pot(
				R"RE(["']poToken["']\s*:\s*["']([^"']+)["'])RE");
			boost::smatch m;
			if (boost::regex_search(html, m, re_pot)) {
				po_token_ = m[1];
				spdlog::debug(
					"Extracted PO Token: {}...", po_token_.substr(0, 20));
			} else {
				spdlog::debug(
					"PO Token not found in webpage via regex (normal if not "
					"served by YouTube)");
			}
		} catch (...) {}
	}

	// Parallel client API calls - all clients called simultaneously
	void fetch_all_clients() {
		if (cancelled) return;

		auto self = shared_from_this();
		const auto &clients = get_clients();
		pending_clients_ = clients.size();

		for (const auto &client : clients) {
			// Create friendly names for logging (matches yt-dlp output)
			std::string friendly_name = client.client_name;
			if (client.client_name == "WEB" &&
				client.user_agent.find("Safari/605") != std::string::npos)
				friendly_name = "web_safari";
			else if (client.client_name == "WEB")
				friendly_name = "web";
			else if (client.client_name == "ANDROID" &&
					 client.device_make.empty())
				friendly_name = "android_sdkless";	// No deviceMake = sdkless
			else if (client.client_name == "ANDROID")
				friendly_name = "android";
			else if (client.client_name == "IOS")
				friendly_name = "ios";
			else if (client.client_name == "TVHTML5")
				friendly_name = "tv";
			else if (client.client_name == "MWEB")
				friendly_name = "mweb";

			spdlog::info(
				"{}: Downloading {} player API JSON", video_id, friendly_name);

			async_get_info_with_client(
				video_id, client,
				[self, name = client.client_name](Result<nlohmann::json> res) {
					if (self->cancelled) return;

					{
						std::lock_guard lock(self->response_mutex_);
						if (res.has_value()) {
							auto &json = res.value();
							if (json.contains("playabilityStatus") &&
								json["playabilityStatus"]["status"] != "OK") {
								spdlog::warn(
									"Video unplayable with client {}: {}", name,
									json["playabilityStatus"]["status"]
										.get<std::string>());
							} else {
								self->responses.emplace_back(
									name, std::move(json));
							}
						}
						self->pending_clients_--;
					}

					// Check if all clients have responded
					if (self->pending_clients_ == 0) { self->finish(); }
				});
		}
	}

	// Mutex for thread-safe response collection
	std::mutex response_mutex_;
	std::atomic<size_t> pending_clients_{0};

	void async_get_info_with_client(
		const std::string &vid, const InnertubeContext &client,
		std::function<void(Result<nlohmann::json>)> callback) {
		std::string api_url = "https://www.youtube.com/youtubei/v1/player";

		// Inject PO Token and Visitor Data for WEB-based clients
		std::string v_data, p_tok;
		if (client.client_name == "WEB" || client.client_name == "MWEB") {
			v_data = web_visitor_data_;
			p_tok = po_token_;
		}

		nlohmann::json payload =
			Innertube::build_context(client, v_data, p_tok);
		payload["videoId"] = vid;
		payload["contentCheckOk"] = true;
		payload["racyCheckOk"] = true;

		auto headers = Innertube::get_headers(client);

		// Add visitor data for TV client (helps with authentication)
		if (client.client_name == "TVHTML5" && !tv_visitor_data_.empty()) {
			headers["X-Goog-Visitor-Id"] = tv_visitor_data_;
		}

		http->async_post(
			api_url, payload.dump(),
			[client_name = client.client_name,
			 cb = std::move(callback)](Result<net::HttpResponse> res_result) {
				if (res_result.has_error()) {
					cb(outcome::failure(res_result.error()));
					return;
				}

				auto r = res_result.value();
				if (r.status_code != 200) {
					cb(outcome::failure(errc::request_failed));
					return;
				}

				auto json = nlohmann::json::parse(r.body, nullptr, false);
				if (json.is_discarded()) {
					cb(outcome::failure(errc::json_parse_error));
				} else {
					cb(json);
				}
			},
			headers);
	}

	// Extract metadata (title, etc) from response JSON
	void extract_video_metadata(const nlohmann::json &json) {
		if (json.contains("videoDetails")) {
			const auto &details = json["videoDetails"];
			collected_info.title = details.value("title", "");
			collected_info.fulltitle = collected_info.title;
			collected_info.description = details.value("shortDescription", "");
			collected_info.uploader = details.value("author", "");
			collected_info.channel = collected_info.uploader;
			collected_info.uploader_id = details.value("channelId", "");
			collected_info.channel_id = collected_info.uploader_id;
			collected_info.channel_url =
				"https://www.youtube.com/channel/" + collected_info.channel_id;
			collected_info.duration = utils::to_number_default<long long>(
				details.value("lengthSeconds", "0"));

			// Format duration string
			long long hrs = collected_info.duration / 3600;
			long long mins = (collected_info.duration % 3600) / 60;
			long long secs = collected_info.duration % 60;
			if (hrs > 0) {
				collected_info.duration_string =
					fmt::format("{}:{:02d}:{:02d}", hrs, mins, secs);
			} else {
				collected_info.duration_string =
					fmt::format("{}:{:02d}", mins, secs);
			}

			collected_info.view_count = utils::to_number_default<long long>(
				details.value("viewCount", "0"));

			// Live status
			collected_info.is_live = details.value("isLive", false);
			collected_info.was_live = details.value("isPostLiveDvr", false);
			if (collected_info.is_live) {
				collected_info.live_status = "is_live";
			} else if (collected_info.was_live) {
				collected_info.live_status = "was_live";
			} else {
				collected_info.live_status = "not_live";
			}

			if (details.contains("thumbnail") &&
				details["thumbnail"].contains("thumbnails")) {
				const auto &thumbs = details["thumbnail"]["thumbnails"];
				if (!thumbs.empty()) {
					collected_info.thumbnail = thumbs.back().value("url", "");
					// Populate thumbnails vector
					for (const auto &t : thumbs) {
						Thumbnail thumb;
						thumb.url = t.value("url", "");
						thumb.width = t.value("width", 0);
						thumb.height = t.value("height", 0);
						collected_info.thumbnails.push_back(thumb);
					}
				}
			}

			// Keywords/tags
			if (details.contains("keywords")) {
				for (const auto &kw : details["keywords"]) {
					if (kw.is_string()) {
						collected_info.tags.push_back(kw.get<std::string>());
					}
				}
			}
		}

		if (json.contains("microformat") &&
			json["microformat"].contains("playerMicroformatRenderer")) {
			const auto &mf = json["microformat"]["playerMicroformatRenderer"];

			auto upload_date_raw = mf.value("uploadDate", "");
			if (!upload_date_raw.empty()) {
				upload_date_raw.erase(std::remove(upload_date_raw.begin(),
												  upload_date_raw.end(), '-'),
									  upload_date_raw.end());
				collected_info.upload_date =
					upload_date_raw.substr(0, 8);  // YYYYMMDD
			}

			// Availability
			collected_info.playable_in_embed =
				mf.value("isPlayableInEmbed", true);

			// Categories
			if (mf.contains("category")) {
				collected_info.categories.push_back(mf.value("category", ""));
			}

			// Family safe determines age limit
			bool family_safe = mf.value("isFamilySafe", true);
			collected_info.age_limit = family_safe ? 0 : 18;

			// Availability status
			bool unlisted = mf.value("isUnlisted", false);
			collected_info.availability = unlisted ? "unlisted" : "public";
		}
	}

	void async_process_fmt(const nlohmann::json &fmt_json,
						   std::function<void(std::optional<VideoFormat>)> cb) {
		VideoFormat fmt = parse_format_metadata(fmt_json);

		// Check signatureCipher
		if (fmt.url.empty() && fmt_json.contains("signatureCipher")) {
			std::string cipher = fmt_json["signatureCipher"];
			std::string s, sp, url_raw;

			size_t pos = 0;
			while (pos < cipher.length()) {
				size_t amp = cipher.find('&', pos);
				if (amp == std::string::npos) amp = cipher.length();
				std::string pair = cipher.substr(pos, amp - pos);
				size_t eq = pair.find('=');
				if (eq != std::string::npos) {
					std::string key = pair.substr(0, eq);
					// Decode value
					std::string val;
					auto decoded =
						boost::urls::decode_view(pair.substr(eq + 1));
					val.assign(decoded.begin(), decoded.end());

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
				decipherer.async_decipher_signature(
					s, [this, url_raw, sp, fmt, cb](std::string sig) mutable {
						std::string final_url_raw = url_raw;
						if (final_url_raw.find('?') == std::string::npos)
							final_url_raw += "?";
						else
							final_url_raw += "&";
						final_url_raw += (sp.empty() ? "sig" : sp) + "=" + sig;

						async_process_url_n(
							final_url_raw,
							[fmt, cb](std::string final_url) mutable {
								fmt.url = final_url;
								cb(fmt);
							});
					});
				return;
			}
		}

		if (!fmt.url.empty()) {
			async_process_url_n(
				fmt.url, [fmt, cb](std::string final_url) mutable {
					fmt.url = final_url;
					cb(fmt);
				});
			return;
		}

		// Skip if no URL found
		cb(std::nullopt);
	}

	void finish() {
		if (responses.empty()) {
			spdlog::error("All clients failed to get video info.");
			return complete(outcome::failure(errc::video_not_found));
		}

		collected_info.id = video_id;
		collected_info.webpage_url =
			"https://www.youtube.com/watch?v=" + video_id;

		// Extract metadata from the first response
		extract_video_metadata(responses[0].second);

		// Collect all format JSONs
		std::vector<nlohmann::json> all_format_jsons;
		for (const auto &[client_name, resp] : responses) {
			if (resp.contains("streamingData")) {
				if (resp["streamingData"].contains("formats")) {
					for (const auto &f : resp["streamingData"]["formats"])
						all_format_jsons.push_back(f);
				}
				if (resp["streamingData"].contains("adaptiveFormats")) {
					for (const auto &f :
						 resp["streamingData"]["adaptiveFormats"])
						all_format_jsons.push_back(f);
				}
			}
		}

		if (all_format_jsons.empty()) { return finalize_formats(); }

		auto pending =
			std::make_shared<std::atomic<size_t>>(all_format_jsons.size());
		auto mut = std::make_shared<std::mutex>();
		auto self = shared_from_this();

		for (const auto &f : all_format_jsons) {
			async_process_fmt(
				f, [self, pending, mut](std::optional<VideoFormat> fmt) {
					if (fmt) {
						std::lock_guard lock(*mut);
						self->collected_info.formats.push_back(*fmt);
					}
					// Check if done
					size_t remaining = pending->fetch_sub(1);
					if (remaining == 1) {
						asio::dispatch(self->handler_ex,
									   [self]() { self->finalize_formats(); });
					}
				});
		}
	}

	void finalize_formats() {
		// Deduplicate formats by itag
		std::set<int> seen_itags;
		std::vector<VideoFormat> unique_formats;
		for (const auto &fmt : collected_info.formats) {
			if (seen_itags.find(fmt.itag) == seen_itags.end()) {
				seen_itags.insert(fmt.itag);
				unique_formats.push_back(fmt);
			}
		}
		collected_info.formats = std::move(unique_formats);

		complete(outcome::success(std::move(collected_info)));
	}
};

const std::vector<InnertubeContext> &AsyncSession::get_clients() {
	// Priority order based on yt-dlp recommendations:
	// 1. android_sdkless - Doesn't require PO Token (best choice)
	// 2. tv              - Good format availability, no PO Token
	// 3. web_safari      - Pre-merged HLS formats, good fallback
	// 4. web             - Standard web client with JS player
	static const std::vector<InnertubeContext> _clients = {
		Innertube::CLIENT_ANDROID_SDKLESS,	// Best: No POT needed
		Innertube::CLIENT_TV,				// Good formats, no POT
		Innertube::CLIENT_WEB_SAFARI,		// HLS formats
		Innertube::CLIENT_WEB};				// Standard fallback
	return _clients;
}

// Extractor Impl
struct Extractor::Impl {
	asio::any_io_executor ex;
	std::shared_ptr<net::HttpClient> http;
	std::shared_ptr<scripting::JsEngine> js;
	std::vector<std::weak_ptr<AsyncSession>> sessions;

	Impl(std::shared_ptr<net::HttpClient> h, asio::any_io_executor ex)
		: ex(std::move(ex)), http(std::move(h)) {
		js = std::make_shared<scripting::JsEngine>(this->ex);
	}

	~Impl() { shutdown(); }

	void shutdown() {
		if (shutdown_flag.exchange(true)) { return; }  // Already shut down

		// Cancel all active sessions
		for (auto &w : sessions) {
			if (auto s = w.lock()) { s->cancel(); }
		}
		sessions.clear();

		// Shutdown the JS engine (this terminates V8)
		if (js) { js->shutdown(); }
	}

	std::atomic<bool> shutdown_flag{false};

	void async_process(std::string url, InfoHandler handler,
					   CompletionExecutor handler_ex) {
		if (shutdown_flag) {
			// Don't start new sessions after shutdown
			asio::dispatch(
				handler_ex, [handler = std::move(handler)]() mutable {
					handler(outcome::failure(errc::request_failed));
				});
			return;
		}

		auto session = std::make_shared<AsyncSession>(
			http, js, std::move(url), std::move(handler),
			std::move(handler_ex));
		sessions.push_back(session);

		sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
									  [](const std::weak_ptr<AsyncSession> &w) {
										  return w.expired();
									  }),
					   sessions.end());

		session->start();
	}
};

Extractor::Extractor(std::shared_ptr<net::HttpClient> http,
					 asio::any_io_executor ex)
	: m_impl(std::make_unique<Impl>(std::move(http), std::move(ex))) {}

Extractor::~Extractor() = default;
Extractor::Extractor(Extractor &&) noexcept = default;
Extractor &Extractor::operator=(Extractor &&) noexcept = default;

asio::any_io_executor Extractor::get_executor() const { return m_impl->ex; }

void Extractor::shutdown() {
	if (m_impl) { m_impl->shutdown(); }
}

void Extractor::async_process_impl(
	std::string url,
	asio::any_completion_handler<void(Result<VideoInfo>)> handler,
	CompletionExecutor handler_ex) {
	m_impl->async_process(
		std::move(url), std::move(handler), std::move(handler_ex));
}

// YouTube Search Implementation

// Search params from yt-dlp (base64 encoded protobuf)
static constexpr const char *SEARCH_PARAMS_VIDEOS =
	"EgIQAfABAQ==";	 // Videos only
static constexpr const char *SEARCH_PARAMS_DATE =
	"CAISAhAB8AEB";	 // Videos, sorted by date

// Parse duration string like "3:33" or "1:23:45" into seconds
static long long parse_duration_string(const std::string &duration_str) {
	if (duration_str.empty()) return 0;

	std::vector<long long> parts;
	long long current = 0;

	for (char c : duration_str) {
		if (c >= '0' && c <= '9') {
			current = current * 10 + (c - '0');
		} else if (c == ':') {
			parts.push_back(current);
			current = 0;
		}
	}
	parts.push_back(current);

	// Convert to seconds: e.g., [3, 33] -> 3*60 + 33 = 213
	long long seconds = 0;
	for (size_t i = 0; i < parts.size(); ++i) {
		long long multiplier = 1;
		for (size_t j = i + 1; j < parts.size(); ++j) { multiplier *= 60; }
		seconds += parts[i] * multiplier;
	}
	return seconds;
}

// Convert seconds to duration string
static std::string seconds_to_duration_string(long long seconds) {
	if (seconds <= 0) return "0:00";

	long long hours = seconds / 3600;
	long long minutes = (seconds % 3600) / 60;
	long long secs = seconds % 60;

	std::string result;
	if (hours > 0) {
		result = std::to_string(hours) + ":";
		if (minutes < 10) result += "0";
	}
	result += std::to_string(minutes) + ":";
	if (secs < 10) result += "0";
	result += std::to_string(secs);
	return result;
}

// Extract search results from Innertube response
static std::vector<SearchResult> extract_search_results(
	const nlohmann::json &response, int max_results) {
	std::vector<SearchResult> results;

	// Navigate to the search results content
	auto contents = utils::traverse_obj<nlohmann::json>(
		response, {"contents", "twoColumnSearchResultsRenderer",
				   "primaryContents", "sectionListRenderer", "contents"});

	if (!contents || !contents->is_array()) return results;

	for (const auto &section : *contents) {
		auto item_section = utils::traverse_obj<nlohmann::json>(
			section, {"itemSectionRenderer", "contents"});
		if (!item_section || !item_section->is_array()) continue;

		for (const auto &item : *item_section) {
			if (static_cast<int>(results.size()) >= max_results) break;

			// Check for videoRenderer
			auto video_renderer =
				utils::traverse_obj<nlohmann::json>(item, {"videoRenderer"});
			if (!video_renderer) continue;

			SearchResult result;

			// Video ID
			if (auto vid = utils::traverse_obj<std::string>(
					*video_renderer, {"videoId"})) {
				result.video_id = *vid;
				result.url = "https://www.youtube.com/watch?v=" + *vid;
			} else {
				continue;  // Skip if no video ID
			}

			// Title
			if (auto title_runs = utils::traverse_obj<nlohmann::json>(
					*video_renderer, {"title", "runs"})) {
				if (title_runs->is_array() && !title_runs->empty()) {
					if (auto text = utils::traverse_obj<std::string>(
							(*title_runs)[0], {"text"})) {
						result.title = *text;
					}
				}
			}

			// Channel
			if (auto channel_runs = utils::traverse_obj<nlohmann::json>(
					*video_renderer, {"ownerText", "runs"})) {
				if (channel_runs->is_array() && !channel_runs->empty()) {
					if (auto text = utils::traverse_obj<std::string>(
							(*channel_runs)[0], {"text"})) {
						result.channel = *text;
					}
					// Channel ID from navigation endpoint
					if (auto browse_id = utils::traverse_obj<std::string>(
							(*channel_runs)[0],
							{"navigationEndpoint", "browseEndpoint",
							 "browseId"})) {
						result.channel_id = *browse_id;
					}
				}
			}

			// Duration
			if (auto duration_text = utils::traverse_obj<std::string>(
					*video_renderer, {"lengthText", "simpleText"})) {
				result.duration_string = *duration_text;
				result.duration_seconds = parse_duration_string(*duration_text);
			}

			// Thumbnail
			if (auto thumbs = utils::traverse_obj<nlohmann::json>(
					*video_renderer, {"thumbnail", "thumbnails"})) {
				if (thumbs->is_array() && !thumbs->empty()) {
					if (auto url = utils::traverse_obj<std::string>(
							thumbs->back(), {"url"})) {
						result.thumbnail = *url;
					}
				}
			}

			// View count
			if (auto view_count_text = utils::traverse_obj<std::string>(
					*video_renderer, {"viewCountText", "simpleText"})) {
				// Parse "1,234,567 views" -> 1234567
				std::string view_str;
				for (char c : *view_count_text) {
					if (c >= '0' && c <= '9') view_str += c;
				}
				if (!view_str.empty()) {
					try {
						result.view_count = std::stoll(view_str);
					} catch (...) {}
				}
			}

			// Published time
			if (auto published = utils::traverse_obj<std::string>(
					*video_renderer, {"publishedTimeText", "simpleText"})) {
				result.upload_date = *published;
			}

			// Description snippet
			if (auto desc_runs = utils::traverse_obj<nlohmann::json>(
					*video_renderer,
					{"detailedMetadataSnippets", 0, "snippetText", "runs"})) {
				if (desc_runs->is_array()) {
					for (const auto &run : *desc_runs) {
						if (auto text = utils::traverse_obj<std::string>(
								run, {"text"})) {
							result.description_snippet += *text;
						}
					}
				}
			}

			results.push_back(std::move(result));
		}
		if (static_cast<int>(results.size()) >= max_results) break;
	}

	return results;
}

void Extractor::async_search_impl(
	SearchOptions options,
	asio::any_completion_handler<void(Result<std::vector<SearchResult>>)>
		handler,
	CompletionExecutor handler_ex) {
	auto http = m_impl->http;

	spdlog::debug("Searching YouTube: \"{}\" (max: {})", options.query,
				  options.max_results);

	// Build search request using Innertube context
	auto context = Innertube::CLIENT_WEB;
	nlohmann::json payload = Innertube::build_context(context);
	payload["query"] = options.query;
	payload["params"] =
		options.sort_by_date ? SEARCH_PARAMS_DATE : SEARCH_PARAMS_VIDEOS;

	auto headers = Innertube::get_headers(context);

	http->async_post(
		"https://www.youtube.com/youtubei/v1/search", payload.dump(),
		[handler = std::move(handler), handler_ex,
		 options](Result<net::HttpResponse> result) mutable {
			if (result.has_error()) {
				asio::dispatch(handler_ex, [handler = std::move(handler),
											err = result.error()]() mutable {
					handler(outcome::failure(err));
				});
				return;
			}

			auto &resp = result.value();
			if (resp.status_code != 200) {
				asio::dispatch(
					handler_ex, [handler = std::move(handler)]() mutable {
						handler(outcome::failure(errc::request_failed));
					});
				return;
			}

			try {
				auto json = nlohmann::json::parse(resp.body);
				auto results =
					extract_search_results(json, options.max_results);
				spdlog::debug("Search found {} results", results.size());

				asio::dispatch(
					handler_ex, [handler = std::move(handler),
								 results = std::move(results)]() mutable {
						handler(outcome::success(std::move(results)));
					});
			} catch (...) {
				asio::dispatch(
					handler_ex, [handler = std::move(handler)]() mutable {
						handler(outcome::failure(errc::json_parse_error));
					});
			}
		},
		headers);
}

std::optional<SearchOptions> parse_search_url(std::string_view url) {
	// Pattern: ytsearch[N|date|all]:query
	// Examples: ytsearch:hello, ytsearch5:hello, ytsearchdate:hello,
	// ytsearchall:hello

	std::string_view prefix = "ytsearch";
	if (url.substr(0, prefix.size()) != prefix) { return std::nullopt; }

	std::string_view remainder = url.substr(prefix.size());

	SearchOptions opts;
	opts.max_results = 1;  // Default for plain ytsearch:
	opts.sort_by_date = false;

	// Find the colon
	auto colon_pos = remainder.find(':');
	if (colon_pos == std::string_view::npos) {
		return std::nullopt;  // No query part
	}

	std::string_view modifier = remainder.substr(0, colon_pos);
	opts.query = std::string(remainder.substr(colon_pos + 1));

	if (opts.query.empty()) { return std::nullopt; }

	if (modifier.empty()) {
		opts.max_results = 1;
	} else if (modifier == "all") {
		opts.max_results = 100;	 // Reasonable max
	} else if (modifier == "date") {
		opts.max_results = 10;
		opts.sort_by_date = true;
	} else {
		// Try to parse as number
		int n = 0;
		for (char c : modifier) {
			if (c >= '0' && c <= '9') {
				n = n * 10 + (c - '0');
			} else {
				// Check for "date" suffix like "5date"
				std::string mod_str(modifier);
				if (mod_str.find("date") != std::string::npos) {
					opts.sort_by_date = true;
					// Extract number before "date"
					auto date_pos = mod_str.find("date");
					if (date_pos > 0) {
						try {
							n = std::stoi(mod_str.substr(0, date_pos));
						} catch (...) { n = 1; }
					}
				} else {
					return std::nullopt;  // Invalid modifier
				}
				break;
			}
		}
		if (n <= 0) n = 1;
		opts.max_results = n;
	}

	return opts;
}

void to_json(nlohmann::json &j, const SearchResult &r) {
	j = nlohmann::json{
		{"id", r.video_id},
		{"title", r.title},
		{"channel", r.channel},
		{"channel_id", r.channel_id},
		{"url", r.url},
		{"duration", r.duration_seconds},
		{"duration_string", r.duration_string},
		{"thumbnail", r.thumbnail},
		{"view_count", r.view_count},
		{"upload_date", r.upload_date},
		{"description", r.description_snippet},
		{"_type", "video"}};
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
	// Convert formats manually
	nlohmann::json formats_json = nlohmann::json::array();
	for (const auto &f : i.formats) {
		nlohmann::json fmt_j;
		to_json(fmt_j, f);
		formats_json.push_back(fmt_j);
	}

	// Convert thumbnails
	nlohmann::json thumbs_json = nlohmann::json::array();
	for (const auto &t : i.thumbnails) {
		thumbs_json.push_back(
			{{"url", t.url}, {"width", t.width}, {"height", t.height}});
	}

	j = nlohmann::json{
		{"id", i.id},
		{"title", i.title},
		{"fulltitle", i.fulltitle},
		{"description", i.description},
		{"uploader", i.uploader},
		{"uploader_id", i.uploader_id},
		{"uploader_url", i.uploader_url},
		{"channel", i.channel},
		{"channel_id", i.channel_id},
		{"channel_url", i.channel_url},
		{"upload_date", i.upload_date},
		{"duration", i.duration},
		{"duration_string", i.duration_string},
		{"view_count", i.view_count},
		{"like_count", i.like_count},
		{"comment_count", i.comment_count},
		{"webpage_url", i.webpage_url},
		{"thumbnail", i.thumbnail},
		{"thumbnails", thumbs_json},
		{"formats", formats_json},
		{"categories", i.categories},
		{"tags", i.tags},
		{"age_limit", i.age_limit},
		{"availability", i.availability},
		{"live_status", i.live_status},
		{"playable_in_embed", i.playable_in_embed},
		{"is_live", i.is_live},
		{"was_live", i.was_live},
		{"extractor", i.extractor},
		{"extractor_key", i.extractor_key},
		{"_type", i._type}};
}

// ... existing methods ...

std::string Extractor::warmup() {
	namespace fs = std::filesystem;
	auto cache_dir = PlayerScript::get_cache_directory();
	spdlog::info("Extracting player from cache dir: {}", cache_dir.string());
	if (!fs::exists(cache_dir)) { return ""; }

	std::string latest_id;
	fs::file_time_type latest_time;
	fs::path latest_path;

	for (const auto &entry : fs::directory_iterator(cache_dir)) {
		if (entry.is_regular_file() && entry.path().extension() == ".js") {
			if (latest_path.empty() || entry.last_write_time() > latest_time) {
				latest_time = entry.last_write_time();
				latest_path = entry.path();
			}
		}
	}

	if (latest_path.empty()) { return ""; }

	latest_id = latest_path.stem().string();

	// Read content
	std::ifstream file(latest_path, std::ios::binary);
	if (!file) { return ""; }
	std::string content((std::istreambuf_iterator<char>(file)),
						std::istreambuf_iterator<char>());

	spdlog::info("Pre-loading cached player {}...", latest_id);

	// Create transient solver and keep it alive via lambda capture
	auto solver = std::make_shared<EjsSolver>(*m_impl->js);
	solver->async_load_player(
		content,
		[solver, id = latest_id](bool success) {
			if (success) {
				spdlog::info("Pre-loaded player {} successfully.", id);
			} else {
				spdlog::warn("Pre-loading player {} failed.", id);
			}
		},
		latest_id);

	return latest_id;
}

}  // namespace ytdlpp::youtube
