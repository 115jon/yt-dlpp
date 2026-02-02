#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <optional>
#include <string>
#include <ytdlpp/http_client.hpp>

#include "po_token.hpp"

namespace ytdlpp {
class EjsSolver;
namespace scripting {
class JsEngine;
}
}  // namespace ytdlpp

namespace ytdlpp::youtube {

/**
 * BotGuard-based PO Token Provider.
 *
 * Implements the Web PO Token flow matching yt-dlp's approach:
 * 1. Fetch BotGuard challenge from YouTube's CreateIntegrityToken endpoint
 * 2. Execute the challenge using V8 JavaScript engine
 * 3. Generate the PO Token from the result
 *
 * This requires a JavaScript runtime (V8) to execute BotGuard challenges.
 */
class BotGuardPoTokenProvider : public PoTokenProvider {
   public:
	explicit BotGuardPoTokenProvider(
		std::shared_ptr<net::HttpClient> http_client,
		scripting::JsEngine *js_engine);

	bool is_available() const override;
	std::string get_name() const override { return "botguard"; }

	std::optional<PoTokenResponse> request_pot(
		const PoTokenRequest &request) override;

   private:
	std::shared_ptr<net::HttpClient> http_client_;
	scripting::JsEngine *js_engine_;
	bool botguard_loaded_{false};

	// Cache the BotGuard interpreter program
	std::string interpreter_program_;

	// Fetch the BotGuard challenge from YouTube
	struct IntegrityTokenResult {
		std::string token;
		std::string program;	// BotGuard program to execute
		std::string challenge;	// Challenge data
		int64_t expires_at{0};
	};

	std::optional<IntegrityTokenResult> fetch_integrity_token(
		const std::string &visitor_data, const std::string &video_id);

	// Execute the BotGuard challenge using V8
	std::optional<std::string> execute_botguard_challenge(
		const std::string &program, const std::string &challenge);

	// Generate the final PO Token
	std::optional<std::string> generate_po_token(
		const std::string &integrity_token, const std::string &mint_result,
		const PoTokenRequest &request);

	// Load BotGuard runtime into V8
	bool load_botguard_runtime();

	// Build request payload for integrity token
	nlohmann::json build_integrity_request(const std::string &visitor_data,
										   const std::string &video_id);
};

}  // namespace ytdlpp::youtube
