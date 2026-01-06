#include <spdlog/spdlog.h>

#include <boost/url.hpp>
#include <mutex>
#include <ytdlpp/extractor.hpp>

#include "../scripting/quickjs_engine.hpp"
#include "decipher.hpp"
#include "innertube.hpp"
#include "player_script.hpp"
#include "utils.hpp"

namespace ytdlpp::youtube {

// Helper to extract video ID
static std::string extract_video_id(const std::string &url_str) {
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

		spdlog::info("Extracting URL: {}", url);
		spdlog::info("{}: Downloading webpage", video_id);

		auto self = shared_from_this();
		player_script.async_fetch(
			video_id, [self](std::optional<std::string> content) {
				if (self->cancelled) return;
				self->on_script(std::move(content));
			});
	}

	void on_script(std::optional<std::string> content) {
		if (cancelled) return;
		if (content) {
			spdlog::info("[jsc:quickjs] Solving JS challenges using quickjs");
			if (!decipherer.load_functions(*content)) {
				spdlog::warn(
					"Failed to load decipher functions. Downloads may fail.");
			}
		} else {
			spdlog::warn(
				"Could not download player script. Signature deciphering "
				"unavailable.");
		}
		fetch_all_clients();  // Parallel API calls
	}

	std::string process_url_n(const std::string &url_raw) {
		try {
			boost::urls::url url_obj(url_raw);
			std::string n_val;
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
				if (it != params.end()) params.replace(it, {"n", new_n});
				return std::string(
					url_obj.buffer().data(), url_obj.buffer().size());
			}
		} catch (...) {}
		return url_raw;
	}

	// Parallel client API calls - all clients called simultaneously
	void fetch_all_clients() {
		if (cancelled) return;

		auto self = shared_from_this();
		const auto &clients = get_clients();
		pending_clients_ = clients.size();

		for (const auto &client : clients) {
			std::string friendly_name = client.client_name;
			if (client.client_name == "WEB")
				friendly_name = "web";
			else if (client.client_name == "ANDROID")
				friendly_name = "android";
			else if (client.client_name == "IOS")
				friendly_name = "ios";

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

		nlohmann::json payload = Innertube::build_context(client);
		payload["videoId"] = vid;
		payload["contentCheckOk"] = true;
		payload["racyCheckOk"] = true;

		auto headers = Innertube::get_headers(client);

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

				try {
					auto json = nlohmann::json::parse(r.body);
					cb(json);
				} catch (...) { cb(outcome::failure(errc::json_parse_error)); }
			},
			headers);
	}

	void finish() {
		if (responses.empty()) {
			spdlog::error("All clients failed to get video info.");
			return complete(outcome::failure(errc::video_not_found));
		}

		// Process formats
		VideoInfo info;
		info.id = video_id;
		info.webpage_url = "https://www.youtube.com/watch?v=" + video_id;

		const nlohmann::json &json =
			responses[0].second;  // Primary metadata source

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

		if (json.contains("microformat") &&
			json["microformat"].contains("playerMicroformatRenderer")) {
			auto upload_date_raw =
				json["microformat"]["playerMicroformatRenderer"].value(
					"uploadDate", "");
			if (!upload_date_raw.empty()) {
				upload_date_raw.erase(std::remove(upload_date_raw.begin(),
												  upload_date_raw.end(), '-'),
									  upload_date_raw.end());
				info.upload_date = upload_date_raw;
			}
		}

		for (const auto &[client_name, resp] : responses) {
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
					std::transform(
						dn_lower.begin(), dn_lower.end(), dn_lower.begin(),
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

				if (fmt.url.empty() && fmt_json.contains("signatureCipher")) {
					std::string cipher = fmt_json["signatureCipher"];
					try {
						std::string s, sp, url_raw;
						auto decode_url =
							[](const std::string &in) -> std::string {
							std::string out;
							// minimal decode
							for (size_t i = 0; i < in.size(); ++i) {
								if (in[i] == '%' && i + 2 < in.size()) {
									std::istringstream is(in.substr(i + 1, 2));
									int v;
									if (is >> std::hex >> v) {
										out += (char)v;
										i += 2;
									} else
										out += in[i];
								} else
									out += in[i];
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
								std::string val =
									decode_url(pair.substr(eq + 1));
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
							fmt.url = process_url_n(url_raw);
						}
					} catch (...) {}
				} else if (!fmt.url.empty()) {
					fmt.url = process_url_n(fmt.url);
				}

				if (fmt.url.empty()) {
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
					for (const auto &f :
						 resp["streamingData"]["adaptiveFormats"])
						process_fmt(f);
				}
			}
			if (skipped_any_due_to_url) {
				spdlog::warn(
					"Some formats skipped due to missing URL (SABR/Server-Side "
					"Ad).");
			}
		}

		complete(info);
	}
};

const std::vector<InnertubeContext> &AsyncSession::get_clients() {
	static const std::vector<InnertubeContext> _clients = {
		Innertube::CLIENT_WEB, Innertube::CLIENT_ANDROID,
		Innertube::CLIENT_IOS};
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

	~Impl() {
		for (auto &w : sessions) {
			if (auto s = w.lock()) { s->cancel(); }
		}
	}

	void async_process(std::string url, InfoHandler handler,
					   CompletionExecutor handler_ex) {
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

void Extractor::async_process_impl(
	std::string url,
	asio::any_completion_handler<void(Result<VideoInfo>)> handler,
	CompletionExecutor handler_ex) {
	m_impl->async_process(
		std::move(url), std::move(handler), std::move(handler_ex));
}

}  // namespace ytdlpp::youtube

namespace ytdlpp {

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

}  // namespace ytdlpp
