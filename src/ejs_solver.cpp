#include <spdlog/spdlog.h>

#include <ytdlpp/ejs_bundle.hpp>
#include <ytdlpp/ejs_solver.hpp>

#include "scripting/js_engine.hpp"

namespace ytdlpp {

EjsSolver::EjsSolver(scripting::JsEngine &js) : js_(&js) {}

bool EjsSolver::ensure_solver_loaded() {
	if (solver_loaded_) return true;

	auto bundle = detail::get_ejs_bundle();
	if (bundle.empty()) {
		spdlog::error("EJS solver bundle is empty");
		return false;
	}

	spdlog::debug("Loading EJS solver bundle ({} bytes)...", bundle.size());
	std::string script =
		"if (!globalThis._ytdlpp_ejs_loaded) { " + std::string(bundle) +
		"; globalThis._ytdlpp_ejs_loaded = true; }";
	auto result = js_->evaluate(script);
	if (result.has_error()) {
		spdlog::debug(
			"Failed to load EJS solver: {}", result.error().message());
		return false;
	}

	solver_loaded_ = true;
	spdlog::debug("EJS solver bundle loaded successfully");
	return true;
}

bool EjsSolver::load_player(const std::string &player_code,
							const std::string &player_id) {
	ready_ = false;

	if (!ensure_solver_loaded()) { return false; }

	// Optimized cache check
	if (!player_id.empty()) {
		auto check = js_->evaluate_and_get(
			"globalThis._loaded_player_id === '" + player_id + "'");
		if (!check.has_error() && check.value() == "true") {
			ready_ = true;
			spdlog::debug("EJS solver used cached player {}", player_id);
			return true;
		}
	}

	// Construct the input JSON for the solver
	nlohmann::json input;
	input["type"] = "player";
	input["player"] = player_code;
	input["requests"] = nlohmann::json::array();  // Empty for preprocessing
	input["output_preprocessed"] = true;

	// Call the jsc function with the input
	std::string call_code =
		"JSON.stringify(jsc(" +
		input.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) +
		"))";
	auto result = js_->evaluate_and_get(call_code);
	if (result.has_error()) {
		spdlog::debug(
			"EJS solver preprocessing failed: {}", result.error().message());
		return false;
	}

	try {
		auto output = nlohmann::json::parse(result.value());
		if (output["type"] == "error") {
			spdlog::debug(
				"EJS solver error: {}", output.value("error", "unknown"));
			return false;
		}

		// Store preprocessed player for future use
		if (output.contains("preprocessed_player")) {
			// Re-evaluate with the preprocessed player stored
			std::string prep_code =
				"globalThis._preprocessed_player = " +
				nlohmann::json(output["preprocessed_player"]).dump() + ";";
			if (!player_id.empty()) {
				prep_code +=
					"globalThis._loaded_player_id = '" + player_id + "';";
			}
			(void)js_->evaluate(prep_code);
		}

		ready_ = true;
		spdlog::debug("EJS solver ready");
		return true;

	} catch (const std::exception &e) {
		spdlog::debug("EJS solver JSON parse error: {}", e.what());
		return false;
	}
}

std::string EjsSolver::solve_sig(const std::string &encrypted_sig) const {
	if (!ready_) return encrypted_sig;

	nlohmann::json input;
	input["type"] = "preprocessed";
	input["preprocessed_player"] = "_preprocessed_player";	// Will be resolved
	input["requests"] = {{{"type", "sig"}, {"challenges", {encrypted_sig}}}};

	// Build the actual call using the stored preprocessed player
	std::string call_code =
		"(function() {"
		"  var input = " +
		input.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) +
		";"
		"  input.preprocessed_player = globalThis._preprocessed_player;"
		"  return JSON.stringify(jsc(input));"
		"})()";

	auto result = js_->evaluate_and_get(call_code);
	if (result.has_error()) {
		spdlog::debug("EJS sig solve failed: {}", result.error().message());
		return encrypted_sig;
	}

	try {
		auto output = nlohmann::json::parse(result.value());
		if (output["type"] == "result" && !output["responses"].empty()) {
			auto &resp = output["responses"][0];
			if (resp["type"] == "result" &&
				resp["data"].contains(encrypted_sig)) {
				return resp["data"][encrypted_sig].get<std::string>();
			}
		}
	} catch (const std::exception &e) {
		spdlog::debug("EJS sig solve JSON error: {}", e.what());
	}

	return encrypted_sig;
}

std::string EjsSolver::solve_n(const std::string &n_param) const {
	if (!ready_) return n_param;

	nlohmann::json input;
	input["type"] = "preprocessed";
	input["preprocessed_player"] = "_preprocessed_player";	// Will be resolved
	input["requests"] = {{{"type", "n"}, {"challenges", {n_param}}}};

	std::string call_code =
		"(function() {"
		"  var input = " +
		input.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) +
		";"
		"  input.preprocessed_player = globalThis._preprocessed_player;"
		"  return JSON.stringify(jsc(input));"
		"})()";

	auto result = js_->evaluate_and_get(call_code);
	if (result.has_error()) {
		spdlog::debug("EJS n solve failed: {}", result.error().message());
		return n_param;
	}

	try {
		auto output = nlohmann::json::parse(result.value());
		if (output["type"] == "result" && !output["responses"].empty()) {
			auto &resp = output["responses"][0];
			if (resp["type"] == "result" && resp["data"].contains(n_param)) {
				return resp["data"][n_param].get<std::string>();
			}
		}
	} catch (const std::exception &e) {
		spdlog::debug("EJS n solve JSON error: {}", e.what());
	}

	return n_param;
}

void EjsSolver::ensure_solver_loaded_async_impl(
	boost::asio::any_completion_handler<void(bool)> handler) {
	if (solver_loaded_) {
		handler(true);
		return;
	}
	auto bundle = detail::get_ejs_bundle();
	if (bundle.empty()) {
		spdlog::error("EJS solver bundle is empty");
		handler(false);
		return;
	}

	spdlog::debug(
		"Async Loading EJS solver bundle ({} bytes)...", bundle.size());
	js_->async_evaluate(
		"if (!globalThis._ytdlpp_ejs_loaded) { " + std::string(bundle) +
			"; globalThis._ytdlpp_ejs_loaded = true; }",
		[this, handler = std::move(handler)](Result<void> res) mutable {
			if (res.has_error()) {
				spdlog::debug(
					"Failed to load EJS solver: {}", res.error().message());
				handler(false);
			} else {
				solver_loaded_ = true;
				spdlog::debug("EJS solver bundle loaded successfully");
				handler(true);
			}
		});
}

void EjsSolver::async_load_player_impl(
	std::string player_code,
	boost::asio::any_completion_handler<void(bool)> handler,
	std::string player_id) {
	ensure_solver_loaded_async_impl(
		[this, player_code = std::move(player_code),
		 handler = std::move(handler), player_id](bool success) mutable {
			if (!success) {
				handler(false);
				return;
			}

			ready_ = false;

			// Optimized cache check logic (Async)
			if (!player_id.empty()) {
				js_->async_evaluate_and_get(
					"globalThis._loaded_player_id === '" + player_id + "'",
					[this, player_id, player_code = std::move(player_code),
					 handler =
						 std::move(handler)](Result<std::string> res) mutable {
						if (!res.has_error() && res.value() == "true") {
							ready_ = true;
							spdlog::debug(
								"EJS solver used cached player {}", player_id);
							handler(true);
							return;
						}
						// Cache miss - proceed to load
						async_load_player_impl_continue(
							std::move(player_code), std::move(handler),
							std::move(player_id));
					});
				return;
			}

			async_load_player_impl_continue(
				std::move(player_code), std::move(handler), "");
		});
}

void EjsSolver::async_load_player_impl_continue(
	std::string player_code,
	boost::asio::any_completion_handler<void(bool)> handler,
	std::string player_id) {
	nlohmann::json input;
	input["type"] = "player";
	input["player"] = player_code;
	input["requests"] = nlohmann::json::array();
	input["output_preprocessed"] = true;

	std::string call_code =
		"JSON.stringify(jsc(" +
		input.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) +
		"))";

	js_->async_evaluate_and_get(
		call_code, [this, handler = std::move(handler),
					player_id](Result<std::string> res) mutable {
			if (res.has_error()) {
				spdlog::debug("EJS solver preprocessing failed: {}",
							  res.error().message());
				handler(false);
				return;
			}

			try {
				auto output = nlohmann::json::parse(res.value());
				if (output["type"] == "error") {
					spdlog::debug("EJS solver error: {}",
								  output.value("error", "unknown"));
					handler(false);
					return;
				}

				if (output.contains("preprocessed_player")) {
					std::string prep_code =
						"globalThis._preprocessed_player = " +
						nlohmann::json(output["preprocessed_player"]).dump() +
						";";
					if (!player_id.empty()) {
						prep_code += "globalThis._loaded_player_id = '" +
									 player_id + "';";
					}
					js_->async_evaluate(
						prep_code, [this, handler = std::move(handler)](
									   Result<void> res2) mutable {
							if (res2.has_error()) {
								handler(false);
							} else {
								ready_ = true;
								spdlog::debug("EJS solver ready");
								handler(true);
							}
						});
				} else {
					ready_ = true;
					handler(true);
				}
			} catch (...) {
				spdlog::debug("EJS solver JSON parse error during async");
				handler(false);
			}
		});
}

void EjsSolver::async_solve_sig_impl(
	std::string sig,
	boost::asio::any_completion_handler<void(std::string)> handler) {
	if (!ready_) {
		handler(sig);
		return;
	}

	nlohmann::json input;
	input["type"] = "preprocessed";
	input["preprocessed_player"] = "_preprocessed_player";
	input["requests"] = {{{"type", "sig"}, {"challenges", {sig}}}};

	std::string call_code =
		"(function() {"
		"  var input = " +
		input.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) +
		";"
		"  input.preprocessed_player = globalThis._preprocessed_player;"
		"  return JSON.stringify(jsc(input));"
		"})()";

	js_->async_evaluate_and_get(
		call_code, [this, sig, handler = std::move(handler)](
					   Result<std::string> res) mutable {
			if (res.has_error()) {
				spdlog::debug(
					"EJS sig solve failed: {}", res.error().message());
				handler(sig);
				return;
			}
			try {
				auto output = nlohmann::json::parse(res.value());
				if (output["type"] == "result" &&
					!output["responses"].empty()) {
					auto &resp = output["responses"][0];
					if (resp["type"] == "result" &&
						resp["data"].contains(sig)) {
						handler(resp["data"][sig].get<std::string>());
						return;
					}
				}
			} catch (...) {}
			handler(sig);
		});
}

void EjsSolver::async_solve_n_impl(
	std::string n,
	boost::asio::any_completion_handler<void(std::string)> handler) {
	if (!ready_) {
		handler(n);
		return;
	}

	nlohmann::json input;
	input["type"] = "preprocessed";
	input["preprocessed_player"] = "_preprocessed_player";
	input["requests"] = {{{"type", "n"}, {"challenges", {n}}}};

	std::string call_code =
		"(function() {"
		"  var input = " +
		input.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) +
		";"
		"  input.preprocessed_player = globalThis._preprocessed_player;"
		"  return JSON.stringify(jsc(input));"
		"})()";

	js_->async_evaluate_and_get(
		call_code, [this, n, handler = std::move(handler)](
					   Result<std::string> res) mutable {
			if (res.has_error()) {
				spdlog::debug("EJS n solve failed: {}", res.error().message());
				handler(n);
				return;
			}
			try {
				auto output = nlohmann::json::parse(res.value());
				if (output["type"] == "result" &&
					!output["responses"].empty()) {
					auto &resp = output["responses"][0];
					if (resp["type"] == "result" && resp["data"].contains(n)) {
						handler(resp["data"][n].get<std::string>());
						return;
					}
				}
			} catch (...) {}
			handler(n);
		});
}

}  // namespace ytdlpp
