#pragma once

#include <boost/asio.hpp>
#include <boost/asio/any_completion_handler.hpp>
#include <memory>
#include <optional>
#include <string>

#include "result.hpp"
#include "types.hpp"

namespace ytdlpp {

class Ytdlpp {
   public:
	Ytdlpp(const Ytdlpp &) = delete;
	Ytdlpp &operator=(const Ytdlpp &) = delete;
	Ytdlpp(Ytdlpp &&) noexcept;
	Ytdlpp &operator=(Ytdlpp &&) noexcept;
	~Ytdlpp();

	// Factory method to create an instance bound to an executor
	static Ytdlpp create(boost::asio::any_io_executor ex);

	// Get the bound executor
	[[nodiscard]] boost::asio::any_io_executor get_executor() const;

	// Async Extract Info
	template <BOOST_ASIO_COMPLETION_TOKEN_FOR(void(Result<ytdlpp::VideoInfo>))
				  CompletionToken>
	auto async_extract(std::string url, CompletionToken &&token) {
		return boost::asio::async_initiate<CompletionToken,
										   void(Result<ytdlpp::VideoInfo>)>(
			[this, url](auto &&handler) mutable {
				extract_impl(std::move(url),
							 boost::asio::any_completion_handler<void(
								 Result<ytdlpp::VideoInfo>)>(
								 std::forward<decltype(handler)>(handler)));
			},
			token);
	}

	// Async Download
	template <BOOST_ASIO_COMPLETION_TOKEN_FOR(void(Result<void>))
				  CompletionToken>
	auto async_download(const ytdlpp::VideoInfo &info, std::string selector,
						std::optional<std::string> merge_fmt,
						ProgressCallback progress_cb, CompletionToken &&token) {
		return boost::asio::async_initiate<CompletionToken, void(Result<void>)>(
			[this, info, selector, merge_fmt,
			 progress_cb](auto &&handler) mutable {
				download_impl(
					info, selector, merge_fmt, progress_cb,
					boost::asio::any_completion_handler<void(Result<void>)>(
						std::forward<decltype(handler)>(handler)));
			},
			token);
	}

   private:
	struct Impl;
	std::shared_ptr<Impl> pimpl_;

	explicit Ytdlpp(std::shared_ptr<Impl> impl);

	void extract_impl(
		std::string url,
		boost::asio::any_completion_handler<void(Result<ytdlpp::VideoInfo>)>
			handler);
	void download_impl(
		ytdlpp::VideoInfo info, std::string selector,
		std::optional<std::string> merge_fmt, ProgressCallback progress_cb,
		boost::asio::any_completion_handler<void(Result<void>)> handler);
};

}  // namespace ytdlpp
