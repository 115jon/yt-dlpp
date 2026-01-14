#include "decipher.hpp"

#include <spdlog/spdlog.h>

#include <ytdlpp/ejs_solver.hpp>

#include "../scripting/native_js_solver.hpp"

namespace ytdlpp::youtube {

SigDecipherer::SigDecipherer(scripting::JsEngine &js)
	: js_(js),
	  native_solver_(std::make_unique<NativeJsSolver>(&js)),
	  ejs_solver_(std::make_unique<EjsSolver>(js)) {}

SigDecipherer::~SigDecipherer() = default;

bool SigDecipherer::load_functions(const std::string &player_code) {
	if (player_code.empty()) {
		spdlog::error("Player code is empty");
		return false;
	}

	spdlog::debug(
		"Loading decipher functions ({} bytes)...", player_code.size());

	// Prioritize EJS Solver (V8 is fast enough)
	// EjsSolver mimics yt-dlp's JS execution logic.
	if (ejs_solver_->load_player(player_code)) {
		spdlog::info(
			"[jsc:ejs] Player script parsed. Solver initialized successfully "
			"(V8).");
		use_ejs_ = true;
		return true;
	}

	spdlog::warn("[jsc:ejs] EJS solver failed, falling back to Native/Regex");

	if (native_solver_->load_player(player_code)) {
		spdlog::info("[jsc:native] Native solver ready");
		use_ejs_ = false;
		return true;
	}

	spdlog::debug("Failed to load decipher functions");
	return false;
}

std::string SigDecipherer::decipher_signature(const std::string &signature) {
	if (use_ejs_) { return ejs_solver_->solve_sig(signature); }
	if (native_solver_->is_ready()) {
		return native_solver_->solve_sig(signature);
	}
	return signature;
}

std::string SigDecipherer::transform_n(const std::string &n) {
	if (use_ejs_) { return ejs_solver_->solve_n(n); }
	if (native_solver_->is_ready()) { return native_solver_->solve_n(n); }
	return n;
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
		[this, code, handler = std::move(handler)](bool success) mutable {
			if (success) {
				spdlog::info(
					"[jsc:ejs] Player script parsed. Solver initialized "
					"successfully (V8).");
				use_ejs_ = true;
				handler(true);
			} else {
				spdlog::warn(
					"[jsc:ejs] EJS solver failed, falling back to "
					"Native/Regex");

				// Native fallback is synchronous currently
				// Assuming it's fast enough for regex or we should post?
				// Regex on 1MB file is debatable.
				// But let's keep it simple for now.
				if (native_solver_->load_player(code)) {
					spdlog::info("[jsc:native] Native solver ready");
					use_ejs_ = false;
					handler(true);
				} else {
					spdlog::debug("Failed to load decipher functions");
					handler(false);
				}
			}
		},
		player_id);
}

void SigDecipherer::async_decipher_signature_impl(
	std::string sig,
	boost::asio::any_completion_handler<void(std::string)> handler) {
	if (use_ejs_) {
		ejs_solver_->async_solve_sig(
			sig, [handler = std::move(handler)](std::string res) mutable {
				handler(std::move(res));
			});
	} else if (native_solver_->is_ready()) {
		// Native sync
		handler(native_solver_->solve_sig(sig));
	} else {
		handler(sig);
	}
}

void SigDecipherer::async_transform_n_impl(
	std::string n,
	boost::asio::any_completion_handler<void(std::string)> handler) {
	if (use_ejs_) {
		ejs_solver_->async_solve_n(
			n, [handler = std::move(handler)](std::string res) mutable {
				handler(std::move(res));
			});
	} else if (native_solver_->is_ready()) {
		handler(native_solver_->solve_n(n));
	} else {
		handler(n);
	}
}

}  // namespace ytdlpp::youtube
