#include "decipher.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
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
	std::lock_guard<std::mutex> lock(cache_mutex_);
	auto it = sig_cache_.find(signature);
	if (it != sig_cache_.end()) {
		spdlog::debug("Signature cache hit: {}...", signature.substr(0, 5));
		return it->second;
	}

	std::string res = ejs_solver_->solve_sig(signature);
	spdlog::debug(
		"Deciphered sig (len {}->{}): {}... -> {}...", signature.size(),
		res.size(), signature.substr(0, std::min(size_t{10}, signature.size())),
		res.substr(0, std::min(size_t{10}, res.size())));
	if (!res.empty()) { sig_cache_[signature] = res; }
	return res;
}

std::string SigDecipherer::transform_n(const std::string &n) {
	std::lock_guard<std::mutex> lock(cache_mutex_);
	auto it = n_cache_.find(n);
	if (it != n_cache_.end()) {
		spdlog::debug("n-parameter cache hit: {}...", n.substr(0, 5));
		return it->second;
	}

	std::string res = ejs_solver_->solve_n(n);
	spdlog::debug(
		"Transformed n (len {}->{}): {} -> {}", n.size(), res.size(), n, res);
	if (!res.empty()) { n_cache_[n] = res; }
	return res;
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

	{
		std::lock_guard<std::mutex> lock(cache_mutex_);
		sig_cache_.clear();
		n_cache_.clear();
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
	{
		std::lock_guard<std::mutex> lock(cache_mutex_);
		auto it = sig_cache_.find(sig);
		if (it != sig_cache_.end()) {
			spdlog::debug("Async signature cache hit: {}...", sig.substr(0, 5));
			handler(it->second);
			return;
		}

		auto pit = pending_sig_.find(sig);
		if (pit != pending_sig_.end()) {
			pit->second.push_back(std::move(handler));
			return;
		}

		pending_sig_[sig].push_back(std::move(handler));
	}

	ejs_solver_->async_solve_sig(sig, [this, sig](std::string res) mutable {
		std::vector<boost::asio::any_completion_handler<void(std::string)>>
			waiters;
		{
			std::lock_guard<std::mutex> lock(cache_mutex_);
			if (!res.empty()) { sig_cache_[sig] = res; }
			auto it = pending_sig_.find(sig);
			if (it != pending_sig_.end()) {
				waiters = std::move(it->second);
				pending_sig_.erase(it);
			}
		}
		for (auto &h : waiters) { h(res); }
	});
}

void SigDecipherer::async_transform_n_impl(
	std::string n,
	boost::asio::any_completion_handler<void(std::string)> handler) {
	{
		std::lock_guard<std::mutex> lock(cache_mutex_);
		auto it = n_cache_.find(n);
		if (it != n_cache_.end()) {
			spdlog::debug("Async n-parameter cache hit: {}...", n.substr(0, 5));
			handler(it->second);
			return;
		}

		auto pit = pending_n_.find(n);
		if (pit != pending_n_.end()) {
			pit->second.push_back(std::move(handler));
			return;
		}

		pending_n_[n].push_back(std::move(handler));
	}

	ejs_solver_->async_solve_n(n, [this, n](std::string res) mutable {
		std::vector<boost::asio::any_completion_handler<void(std::string)>>
			waiters;
		{
			std::lock_guard<std::mutex> lock(cache_mutex_);
			if (!res.empty()) { n_cache_[n] = res; }
			auto it = pending_n_.find(n);
			if (it != pending_n_.end()) {
				waiters = std::move(it->second);
				pending_n_.erase(it);
			}
		}
		for (auto &h : waiters) { h(res); }
	});
}

}  // namespace ytdlpp::youtube
