#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <ytdlpp/result.hpp>

namespace ytdlpp::scripting {

class JsEngine {
   public:
	explicit JsEngine(boost::asio::any_io_executor ex);
	~JsEngine();

	/// Shutdown the V8 engine, terminating any running scripts and stopping the
	/// worker thread.
	void shutdown();

	// Async evaluators
	template <typename CompletionToken>
	auto async_evaluate(std::string code, CompletionToken &&token) {
		return boost::asio::async_initiate<CompletionToken, void(Result<void>)>(
			[this](auto handler, std::string code) {
				async_evaluate_impl(std::move(code), std::move(handler));
			},
			token, std::move(code));
	}

	template <typename CompletionToken>
	auto async_evaluate_and_get(std::string code, CompletionToken &&token) {
		return boost::asio::async_initiate<CompletionToken,
										   void(Result<std::string>)>(
			[this](auto handler, std::string code) {
				async_evaluate_and_get_impl(
					std::move(code), std::move(handler));
			},
			token, std::move(code));
	}

	template <typename CompletionToken>
	auto async_call_function(std::string func_name,
							 std::vector<std::string> args,
							 CompletionToken &&token) {
		return boost::asio::async_initiate<CompletionToken,
										   void(Result<std::string>)>(
			[this](auto handler, std::string func_name,
				   std::vector<std::string> args) {
				async_call_function_impl(
					std::move(func_name), std::move(args), std::move(handler));
			},
			token, std::move(func_name), std::move(args));
	}

	// Keep synchronous versions for non-critical paths or fallback
	Result<void> evaluate(const std::string &code);
	Result<std::string> call_function(const std::string &func_name,
									  const std::vector<std::string> &args);
	Result<std::string> evaluate_and_get(const std::string &code);

   private:
	struct Impl;
	std::unique_ptr<Impl> impl_;

	void async_evaluate_impl(
		std::string code,
		boost::asio::any_completion_handler<void(Result<void>)> handler);

	void async_evaluate_and_get_impl(
		std::string code,
		boost::asio::any_completion_handler<void(Result<std::string>)> handler);

	void async_call_function_impl(
		std::string func_name, std::vector<std::string> args,
		boost::asio::any_completion_handler<void(Result<std::string>)> handler);
};

}  // namespace ytdlpp::scripting
