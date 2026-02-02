#include "po_token_provider.hpp"

#include <spdlog/spdlog.h>

#include <boost/algorithm/string.hpp>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/url/decode_view.hpp>
#include <chrono>
#include <random>
#include <ytdlpp/result.hpp>

#include "scripting/js_engine.hpp"

namespace ytdlpp::youtube {

namespace beast_base64 = boost::beast::detail::base64;

// =============================================================================
// Helper Functions
// =============================================================================

static std::string generate_session_id() {
	static std::random_device rd;
	static std::mt19937 gen(rd());
	static std::uniform_int_distribution<> dis(0, 15);

	std::string session_id;
	session_id.reserve(32);
	const char *hex = "0123456789abcdef";
	for (int i = 0; i < 32; ++i) { session_id += hex[dis(gen)]; }
	return session_id;
}

static std::string base64_encode_str(const std::string &input) {
	std::string encoded;
	encoded.resize(beast_base64::encoded_size(input.size()));
	beast_base64::encode(encoded.data(), input.data(), input.size());
	return encoded;
}

static std::string base64_decode_str(const std::string &input) {
	std::vector<uint8_t> decoded;
	decoded.resize(beast_base64::decoded_size(input.size()));
	auto [out_len, _] =
		beast_base64::decode(decoded.data(), input.data(), input.size());
	decoded.resize(out_len);
	return std::string(decoded.begin(), decoded.end());
}

// =============================================================================
// BotGuardPoTokenProvider Implementation
// =============================================================================

BotGuardPoTokenProvider::BotGuardPoTokenProvider(
	std::shared_ptr<net::HttpClient> http_client,
	scripting::JsEngine *js_engine)
	: http_client_(std::move(http_client)), js_engine_(js_engine) {
	// WebPO supports these contexts and clients
	supported_contexts_ = {
		PoTokenContext::GVS, PoTokenContext::PLAYER, PoTokenContext::SUBS};
	supported_clients_ = {
		"WEB",
		"MWEB",
		"TVHTML5",
		"WEB_EMBEDDED_PLAYER",
		"WEB_CREATOR",
		"WEB_REMIX",
		"TVHTML5_SIMPLY",
		"TVHTML5_SIMPLY_EMBEDDED_PLAYER"};
}

bool BotGuardPoTokenProvider::is_available() const {
	return http_client_ != nullptr && js_engine_ != nullptr;
}

std::optional<PoTokenResponse> BotGuardPoTokenProvider::request_pot(
	const PoTokenRequest &request) {
	if (!is_available()) {
		spdlog::debug("BotGuardPoTokenProvider: Not available");
		return std::nullopt;
	}

	std::string visitor_data = request.visitor_data;
	if (visitor_data.empty()) {
		spdlog::debug("BotGuardPoTokenProvider: No visitor data");
		return std::nullopt;
	}

	spdlog::debug(
		"BotGuardPoTokenProvider: Requesting PO Token for visitor={}...",
		visitor_data.substr(0, 20));

	// Step 1: Fetch integrity token from YouTube
	auto integrity_result =
		fetch_integrity_token(visitor_data, request.video_id);
	if (!integrity_result) {
		spdlog::warn(
			"BotGuardPoTokenProvider: Failed to fetch integrity token");
		return std::nullopt;
	}

	spdlog::debug(
		"BotGuardPoTokenProvider: Got integrity token, program_len={}",
		integrity_result->program.size());

	// Step 2: Execute BotGuard challenge if we have a program
	std::string mint_result;
	if (!integrity_result->program.empty()) {
		auto challenge_result = execute_botguard_challenge(
			integrity_result->program, integrity_result->challenge);
		if (challenge_result) {
			mint_result = *challenge_result;
		} else {
			spdlog::debug(
				"BotGuardPoTokenProvider: Challenge execution failed, using "
				"fallback");
		}
	}

	// Step 3: Generate final PO Token
	auto po_token =
		generate_po_token(integrity_result->token, mint_result, request);
	if (!po_token) {
		spdlog::warn("BotGuardPoTokenProvider: Failed to generate PO Token");
		return std::nullopt;
	}

	spdlog::info("BotGuardPoTokenProvider: Generated PO Token successfully");

	PoTokenResponse response;
	response.po_token = *po_token;
	response.expires_at = integrity_result->expires_at;
	return response;
}

nlohmann::json BotGuardPoTokenProvider::build_integrity_request(
	const std::string &visitor_data, const std::string &video_id) {
	nlohmann::json payload;

	// Build context matching yt-dlp's WEB client
	payload["context"]["client"]["clientName"] = "WEB";
	payload["context"]["client"]["clientVersion"] = "2.20250128.00.00";
	payload["context"]["client"]["hl"] = "en";
	payload["context"]["client"]["gl"] = "US";
	payload["context"]["client"]["visitorData"] = visitor_data;
	payload["context"]["client"]["userAgent"] =
		"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
		"(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

	payload["context"]["request"]["internalExperimentFlags"] =
		nlohmann::json::array();
	payload["context"]["request"]["consistencyTokenJars"] =
		nlohmann::json::array();

	// Add video ID for content binding if provided
	if (!video_id.empty()) { payload["videoId"] = video_id; }

	return payload;
}

std::optional<BotGuardPoTokenProvider::IntegrityTokenResult>
BotGuardPoTokenProvider::fetch_integrity_token(const std::string &visitor_data,
											   const std::string &video_id) {
	// YouTube's integrity token endpoint
	std::string api_url =
		"https://jnn-pa.googleapis.com/v1/createIntegrityToken";

	// Add API key
	api_url += "?key=AIzaSyDCU8hByM-4DrUqRUYnGn-3llEO78bcxq8";

	nlohmann::json payload = build_integrity_request(visitor_data, video_id);

	// Request headers
	std::map<std::string, std::string> headers = {
		{"Content-Type", "application/json+protobuf"},
		{"User-Agent",
		 "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
		 "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"},
		{"X-Goog-Api-Key", "AIzaSyDCU8hByM-4DrUqRUYnGn-3llEO78bcxq8"},
		{"X-User-Agent", "grpc-web-javascript/0.1"},
		{"Origin", "https://www.youtube.com"},
		{"Referer", "https://www.youtube.com/"}};

	// Make sync request via promise
	std::promise<Result<net::HttpResponse>> promise;
	auto future = promise.get_future();

	// Simple request body for CreateIntegrityToken
	// Based on analysis of yt-dlp and browser requests
	nlohmann::json request_body = nlohmann::json::array();
	request_body.push_back(visitor_data);

	http_client_->async_post(
		api_url, request_body.dump(),
		[&promise](Result<net::HttpResponse> res) {
			promise.set_value(std::move(res));
		},
		headers);

	auto status = future.wait_for(std::chrono::seconds(30));
	if (status != std::future_status::ready) {
		spdlog::warn(
			"BotGuardPoTokenProvider: Timeout fetching integrity token");
		return std::nullopt;
	}

	auto http_result = future.get();
	if (http_result.has_error()) {
		spdlog::debug(
			"BotGuardPoTokenProvider: HTTP error fetching integrity token");
		return std::nullopt;
	}

	auto &response = http_result.value();
	if (response.status_code != 200) {
		spdlog::debug(
			"BotGuardPoTokenProvider: HTTP {} from integrity endpoint: {}",
			response.status_code, response.body.substr(0, 200));
		return std::nullopt;
	}

	// Parse response
	try {
		auto json = nlohmann::json::parse(response.body, nullptr, false);
		if (json.is_discarded()) {
			spdlog::debug("BotGuardPoTokenProvider: Invalid JSON response");
			return std::nullopt;
		}

		IntegrityTokenResult token_result;

		// Response format varies - try different paths
		if (json.is_array() && json.size() > 0) {
			// Array format: ["token", optionalProgram, optionalChallenge]
			token_result.token = json[0].get<std::string>();
			if (json.size() > 1 && json[1].is_string()) {
				token_result.program = json[1].get<std::string>();
			}
			if (json.size() > 2 && json[2].is_string()) {
				token_result.challenge = json[2].get<std::string>();
			}
		} else if (json.contains("integrityToken")) {
			// Object format
			token_result.token = json["integrityToken"].get<std::string>();
			if (json.contains("program")) {
				token_result.program = json["program"].get<std::string>();
			}
			if (json.contains("challenge")) {
				token_result.challenge = json["challenge"].get<std::string>();
			}
		}

		if (token_result.token.empty()) {
			spdlog::debug("BotGuardPoTokenProvider: No token in response");
			return std::nullopt;
		}

		// Default TTL of 6 hours
		token_result.expires_at =
			std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::system_clock::now().time_since_epoch())
				.count() +
			21600;

		return token_result;
	} catch (const std::exception &e) {
		spdlog::debug("BotGuardPoTokenProvider: Exception parsing response: {}",
					  e.what());
		return std::nullopt;
	}
}

std::optional<std::string> BotGuardPoTokenProvider::execute_botguard_challenge(
	const std::string &program, const std::string &challenge) {
	// For now, return empty - full BotGuard VM execution is complex
	// The token from integrity endpoint often works without challenge execution
	// for many requests

	if (program.empty()) { return std::nullopt; }

	// TODO: Implement full BotGuard VM execution
	// This would require:
	// 1. Loading the BotGuard VM script
	// 2. Executing the program with the challenge
	// 3. Returning the result

	spdlog::debug(
		"BotGuardPoTokenProvider: BotGuard challenge execution not "
		"implemented, using integrity token directly");

	return std::nullopt;
}

std::optional<std::string> BotGuardPoTokenProvider::generate_po_token(
	const std::string &integrity_token, const std::string &mint_result,
	const PoTokenRequest &request) {
	// Get content binding based on context
	WebPoCacheSpecProvider cache_provider;
	auto [content_binding, binding_type] =
		cache_provider.get_content_binding(request, true);

	if (content_binding.empty()) {
		spdlog::debug("BotGuardPoTokenProvider: No content binding");
		return std::nullopt;
	}

	// Build token components
	std::string token_data = integrity_token;

	// If we have mint result from BotGuard execution, use it
	if (!mint_result.empty()) { token_data += ":" + mint_result; }

	// The actual PO Token for YouTube is the integrity token itself
	// for most cases when BotGuard challenge execution isn't required
	return integrity_token;
}

bool BotGuardPoTokenProvider::load_botguard_runtime() {
	// BotGuard runtime loading would go here
	// This is complex and requires fetching/caching the VM script
	return false;
}

}  // namespace ytdlpp::youtube
