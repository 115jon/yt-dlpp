#include "decipher.hpp"

#include <spdlog/spdlog.h>

#include <utility>
#include <ytdlpp/ejs_solver.hpp>

namespace ytdlpp::youtube {

SigDecipherer::SigDecipherer(scripting::JsEngine &js)
	: js_(js), ejs_solver_(std::make_unique<EjsSolver>(js)) {}

SigDecipherer::~SigDecipherer() = default;

bool SigDecipherer::load_functions(const std::string &player_code) {
	if (player_code.empty()) {
		spdlog::error("Player code is empty");
		return false;
	}

	spdlog::debug(
		"Loading decipher functions ({} bytes)...", player_code.size());

	if (ejs_solver_->load_player(player_code)) {
		spdlog::info(
			"[jsc:ejs] Player script parsed. Solver initialized successfully "
			"(V8).");
		return true;
	}

	spdlog::debug("Failed to load decipher functions");
	return false;
}

std::string SigDecipherer::decipher_signature(const std::string &signature) {
	return ejs_solver_->solve_sig(signature);
}

std::string SigDecipherer::transform_n(const std::string &n) {
	return ejs_solver_->solve_n(n);
}

// Async impls
void SigDecipherer::async_load_functions_impl(
	std::string code, boost::asio::any_completion_handler<void(bool)> handler,
	std::string player_id) {
	if (code.empty()) {
		spdlog::error("Player code is empty");
		handler(false);
		return;
	}

	spdlog::debug("Async loading decipher functions ({} bytes, id: {})...",
				  code.size(), player_id);

	ejs_solver_->async_load_player(
		code,
		[handler = std::move(handler)](bool success) mutable {
			if (success) {
				spdlog::info(
					"[jsc:ejs] Player script parsed. Solver initialized "
					"successfully (V8).");
				handler(true);
			} else {
				spdlog::debug("Failed to load decipher functions");
				handler(false);
			}
		},
		player_id);
}

void SigDecipherer::async_decipher_signature_impl(
	std::string sig,
	boost::asio::any_completion_handler<void(std::string)> handler) {
	ejs_solver_->async_solve_sig(
		std::move(sig),
		[handler = std::move(handler)](std::string res) mutable {
			handler(std::move(res));
		});
}

void SigDecipherer::async_transform_n_impl(
	std::string n,
	boost::asio::any_completion_handler<void(std::string)> handler) {
	ejs_solver_->async_solve_n(
		std::move(n), [handler = std::move(handler)](std::string res) mutable {
			handler(std::move(res));
		});
}

}  // namespace ytdlpp::youtube
