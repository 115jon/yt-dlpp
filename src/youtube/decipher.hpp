#pragma once

#include <memory>
#include <string>

#include "../scripting/js_engine.hpp"

// Forward declaration
namespace ytdlpp {
class NativeJsSolver;
class EjsSolver;
}  // namespace ytdlpp

namespace ytdlpp::youtube {

class SigDecipherer {
   public:
	explicit SigDecipherer(scripting::JsEngine &js);
	~SigDecipherer();

	// Non-copyable, non-movable
	SigDecipherer(const SigDecipherer &) = delete;
	SigDecipherer &operator=(const SigDecipherer &) = delete;
	SigDecipherer(SigDecipherer &&) = delete;
	SigDecipherer &operator=(SigDecipherer &&) = delete;

	// Load decipher functions from player code
	bool load_functions(const std::string &player_code);

	// Decipher a signature
	std::string decipher_signature(const std::string &signature);

	// Transform n-parameter
	std::string transform_n(const std::string &n);

	// Load decipher functions (async)
	template <typename CompletionToken>
	auto async_load_functions(std::string player_code, CompletionToken &&token,
							  std::string player_id = "") {
		return boost::asio::async_initiate<CompletionToken, void(bool)>(
			[this](auto handler, std::string code, std::string id) {
				async_load_functions_impl(
					std::move(code), std::move(handler), std::move(id));
			},
			token, std::move(player_code), std::move(player_id));
	}

	template <typename CompletionToken>
	auto async_decipher_signature(std::string signature,
								  CompletionToken &&token) {
		return boost::asio::async_initiate<CompletionToken, void(std::string)>(
			[this](auto handler, std::string sig) {
				async_decipher_signature_impl(
					std::move(sig), std::move(handler));
			},
			token, std::move(signature));
	}

	template <typename CompletionToken>
	auto async_transform_n(std::string n, CompletionToken &&token) {
		return boost::asio::async_initiate<CompletionToken, void(std::string)>(
			[this](auto handler, std::string n) {
				async_transform_n_impl(std::move(n), std::move(handler));
			},
			token, std::move(n));
	}

   private:
	scripting::JsEngine &js_;
	std::unique_ptr<NativeJsSolver> native_solver_;
	std::unique_ptr<EjsSolver> ejs_solver_;
	bool use_ejs_{false};

	// Async impls
	void async_load_functions_impl(
		std::string code,
		boost::asio::any_completion_handler<void(bool)> handler,
		std::string player_id);
	void async_decipher_signature_impl(
		std::string sig,
		boost::asio::any_completion_handler<void(std::string)> handler);
	void async_transform_n_impl(
		std::string n,
		boost::asio::any_completion_handler<void(std::string)> handler);
};

}  // namespace ytdlpp::youtube
