#pragma once

#include <boost/asio/any_completion_executor.hpp>
#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <ytdlpp/result.hpp>
#include <ytdlpp/types.hpp>

// Forward declarations
namespace ytdlpp::net {
class HttpClient;
}

namespace ytdlpp::youtube {

namespace asio = boost::asio;

class Extractor {
   public:
	Extractor(const Extractor &) = delete;
	Extractor &operator=(const Extractor &) = delete;
	Extractor(Extractor &&) noexcept;
	Extractor &operator=(Extractor &&) noexcept;
	~Extractor();

	explicit Extractor(std::shared_ptr<ytdlpp::net::HttpClient> http,
					   asio::any_io_executor ex);

	[[nodiscard]] asio::any_io_executor get_executor() const;

	using CompletionExecutor = asio::any_completion_executor;

	template <BOOST_ASIO_COMPLETION_TOKEN_FOR(void(Result<VideoInfo>))
				  CompletionToken>
	auto async_process(std::string_view url, CompletionToken &&token) {
		auto ex = get_executor();
		return asio::async_initiate<CompletionToken, void(Result<VideoInfo>)>(
			[this, ex, url_s = std::string(url)](auto &&handler) mutable {
				CompletionExecutor handler_ex =
					asio::get_associated_executor(handler, ex);

				auto any_handler =
					asio::any_completion_handler<void(Result<VideoInfo>)>{
						std::forward<decltype(handler)>(handler)};

				async_process_impl(std::move(url_s), std::move(any_handler),
								   std::move(handler_ex));
			},
			token);
	}

   private:
	struct Impl;

	void async_process_impl(
		std::string url,
		asio::any_completion_handler<void(Result<VideoInfo>)> handler,
		CompletionExecutor handler_ex);

	std::unique_ptr<Impl> m_impl;
};

// JSON Serialization
void to_json(nlohmann::json &j, const VideoFormat &f);
void to_json(nlohmann::json &j, const VideoInfo &i);

}  // namespace ytdlpp::youtube
