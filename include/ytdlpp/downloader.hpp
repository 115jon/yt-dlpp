#pragma once

#include <boost/asio/any_completion_executor.hpp>
#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <ytdlpp/result.hpp>
#include <ytdlpp/types.hpp>

// Forward declarations
namespace ytdlpp::net {
class HttpClient;
}

namespace ytdlpp {

namespace asio = boost::asio;

class Downloader {
   public:
	Downloader(const Downloader &) = delete;
	Downloader &operator=(const Downloader &) = delete;
	Downloader(Downloader &&) noexcept;
	Downloader &operator=(Downloader &&) noexcept;
	~Downloader();

	explicit Downloader(std::shared_ptr<net::HttpClient> http);

	[[nodiscard]] asio::any_io_executor get_executor() const;

	using CompletionExecutor = asio::any_io_executor;

	struct StreamInfo {
		const VideoFormat *video{};
		const VideoFormat *audio{};
	};

	// Static helper - doesn't need async
	[[nodiscard]] static StreamInfo select_streams(const VideoInfo &info,
												   std::string_view selector);

	template <BOOST_ASIO_COMPLETION_TOKEN_FOR(void(Result<std::string>))
				  CompletionToken>
	auto async_download(const VideoInfo &info, std::string_view format_selector,
						std::optional<std::string> merge_format,
						ProgressCallback progress_cb, CompletionToken &&token) {
		auto ex = get_executor();
		return asio::async_initiate<CompletionToken, void(Result<std::string>)>(
			[this, ex, info, format_selector_s = std::string(format_selector),
			 merge_format = std::move(merge_format),
			 progress_cb = std::move(progress_cb)](auto &&handler) mutable {
				CompletionExecutor handler_ex =
					asio::get_associated_executor(handler, ex);

				auto any_handler =
					asio::any_completion_handler<void(Result<std::string>)>{
						std::forward<decltype(handler)>(handler)};

				async_download_impl(
					info, std::move(format_selector_s), std::move(merge_format),
					std::move(progress_cb), std::move(any_handler),
					std::move(handler_ex));
			},
			token);
	}

   private:
	struct Impl;

	void async_download_impl(
		const VideoInfo &info, std::string format_selector,
		std::optional<std::string> merge_format, ProgressCallback progress_cb,
		asio::any_completion_handler<void(Result<std::string>)> handler,
		CompletionExecutor handler_ex);

	std::unique_ptr<Impl> m_impl;
};

}  // namespace ytdlpp
