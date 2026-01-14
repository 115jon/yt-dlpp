#pragma once

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include <string>

// Forward declaration
namespace ytdlpp::scripting {
class JsEngine;
}  // namespace ytdlpp::scripting

namespace ytdlpp {

/**
 * EJS Solver - JavaScript Challenge Solver using yt-dlp's EJS approach.
 *
 * This uses the AST-based solver from yt-dlp that parses the YouTube player
 * script with meriyah, extracts the signature and n-parameter functions via
 * AST pattern matching, and generates solver functions.
 *
 * Much more robust than simple string matching since it handles code
 * minification and obfuscation.
 */
class EjsSolver {
   public:
	explicit EjsSolver(scripting::JsEngine &js);

	// Async versions
	// Async version of load_player
	template <typename CompletionToken>
	auto async_load_player(std::string player_code, CompletionToken &&token,
						   std::string player_id = "") {
		return boost::asio::async_initiate<CompletionToken, void(bool)>(
			[this](auto handler, std::string code, std::string id) {
				async_load_player_impl(
					std::move(code), std::move(handler), std::move(id));
			},
			token, std::move(player_code), std::move(player_id));
	}

	template <typename CompletionToken>
	auto async_solve_sig(std::string sig, CompletionToken &&token) {
		return boost::asio::async_initiate<CompletionToken, void(std::string)>(
			[this](auto handler, std::string sig) {
				async_solve_sig_impl(std::move(sig), std::move(handler));
			},
			token, std::move(sig));
	}

	template <typename CompletionToken>
	auto async_solve_n(std::string n, CompletionToken &&token) {
		return boost::asio::async_initiate<CompletionToken, void(std::string)>(
			[this](auto handler, std::string n) {
				async_solve_n_impl(std::move(n), std::move(handler));
			},
			token, std::move(n));
	}

	// Load and preprocess the player script.
	bool load_player(const std::string &player_code,
					 const std::string &player_id = "");

	// Solve a signature challenge.
	std::string solve_sig(const std::string &encrypted_sig) const;

	// Solve an n-parameter challenge.
	std::string solve_n(const std::string &n_param) const;

	[[nodiscard]] bool is_ready() const { return ready_; }

   private:
	scripting::JsEngine *js_;
	bool ready_{false};
	bool solver_loaded_{false};

	// Internal handling
	void async_load_player_impl(
		std::string player_code,
		boost::asio::any_completion_handler<void(bool)> handler,
		std::string player_id);

	void async_load_player_impl_continue(
		std::string player_code,
		boost::asio::any_completion_handler<void(bool)> handler,
		std::string player_id);

	void async_solve_sig_impl(
		std::string sig,
		boost::asio::any_completion_handler<void(std::string)> handler);

	void async_solve_n_impl(
		std::string n,
		boost::asio::any_completion_handler<void(std::string)> handler);

	// Helper for checking/loading bundle async
	template <typename Handler>
	void ensure_solver_loaded_async(Handler &&handler) {
		// If loaded, call handler(true) immediately via post?
		// Logic to be implemented in cpp
	}

	void ensure_solver_loaded_async_impl(
		boost::asio::any_completion_handler<void(bool)> handler);

	bool ensure_solver_loaded();
};

}  // namespace ytdlpp
