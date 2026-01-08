#pragma once

#include <ytdlpp/ytdlpp_export.h>

#include <boost/asio/any_completion_executor.hpp>
#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "result.hpp"


namespace ytdlpp::net {

namespace asio = boost::asio;

struct YTDLPP_EXPORT HttpResponse {
	int status_code;
	std::string body;
	std::map<std::string, std::string> headers;
};

class YTDLPP_EXPORT HttpClient {
   public:
	HttpClient(const HttpClient &) = delete;
	HttpClient &operator=(const HttpClient &) = delete;
	HttpClient(HttpClient &&) noexcept;
	HttpClient &operator=(HttpClient &&) noexcept;
	~HttpClient();

	explicit HttpClient(asio::any_io_executor ex);

	[[nodiscard]] asio::any_io_executor get_executor() const;

	using CompletionExecutor = asio::any_io_executor;
	using ProgressCallback =
		std::function<void(long long dl_now, long long dl_total)>;

	// Async GET
	template <BOOST_ASIO_COMPLETION_TOKEN_FOR(void(Result<HttpResponse>))
				  CompletionToken>
	auto async_get(std::string_view url, CompletionToken &&token,
				   std::map<std::string, std::string> headers = {}) {
		auto ex = get_executor();
		return asio::async_initiate<CompletionToken,
									void(Result<HttpResponse>)>(
			[this, ex, url_s = std::string(url),
			 headers = std::move(headers)](auto &&handler) mutable {
				CompletionExecutor handler_ex =
					asio::get_associated_executor(handler, ex);

				auto any_handler =
					asio::any_completion_handler<void(Result<HttpResponse>)>{
						std::forward<decltype(handler)>(handler)};

				async_get_impl(std::move(url_s), std::move(headers),
							   std::move(any_handler), std::move(handler_ex));
			},
			token);
	}

	// Async POST
	template <BOOST_ASIO_COMPLETION_TOKEN_FOR(void(Result<HttpResponse>))
				  CompletionToken>
	auto async_post(std::string_view url, std::string body,
					CompletionToken &&token,
					std::map<std::string, std::string> headers = {}) {
		auto ex = get_executor();
		return asio::async_initiate<CompletionToken,
									void(Result<HttpResponse>)>(
			[this, ex, url_s = std::string(url), body = std::move(body),
			 headers = std::move(headers)](auto &&handler) mutable {
				CompletionExecutor handler_ex =
					asio::get_associated_executor(handler, ex);

				auto any_handler =
					asio::any_completion_handler<void(Result<HttpResponse>)>{
						std::forward<decltype(handler)>(handler)};

				async_post_impl(
					std::move(url_s), std::move(body), std::move(headers),
					std::move(any_handler), std::move(handler_ex));
			},
			token);
	}

	// Async Download File
	template <BOOST_ASIO_COMPLETION_TOKEN_FOR(void(Result<void>))
				  CompletionToken>
	auto async_download_file(std::string_view url, std::string_view output_path,
							 ProgressCallback progress_cb,
							 CompletionToken &&token) {
		auto ex = get_executor();
		return asio::async_initiate<CompletionToken, void(Result<void>)>(
			[this, ex, url_s = std::string(url),
			 output_path_s = std::string(output_path),
			 progress_cb = std::move(progress_cb)](auto &&handler) mutable {
				CompletionExecutor handler_ex =
					asio::get_associated_executor(handler, ex);

				auto any_handler =
					asio::any_completion_handler<void(Result<void>)>{
						std::forward<decltype(handler)>(handler)};

				async_download_file_impl(
					std::move(url_s), std::move(output_path_s),
					std::move(progress_cb), std::move(any_handler),
					std::move(handler_ex));
			},
			token);
	}

   private:
	struct Impl;

	void async_get_impl(
		std::string url, std::map<std::string, std::string> headers,
		asio::any_completion_handler<void(Result<HttpResponse>)> handler,
		CompletionExecutor handler_ex);

	void async_post_impl(
		std::string url, std::string body,
		std::map<std::string, std::string> headers,
		asio::any_completion_handler<void(Result<HttpResponse>)> handler,
		CompletionExecutor handler_ex);

	void async_download_file_impl(
		std::string url, std::string output_path, ProgressCallback progress_cb,
		asio::any_completion_handler<void(Result<void>)> handler,
		CompletionExecutor handler_ex);

	std::unique_ptr<Impl> m_impl;
};

}  // namespace ytdlpp::net
