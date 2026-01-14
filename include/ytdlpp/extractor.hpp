#pragma once

#include <ytdlpp/ytdlpp_export.h>

#include <boost/asio/any_completion_executor.hpp>
#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <ytdlpp/result.hpp>
#include <ytdlpp/types.hpp>

// Forward declarations
namespace ytdlpp::net {
class HttpClient;
}  // namespace ytdlpp::net

namespace ytdlpp::youtube {

namespace asio = boost::asio;

class YTDLPP_EXPORT Extractor {
   public:
	Extractor(const Extractor &) = delete;
	Extractor &operator=(const Extractor &) = delete;
	Extractor(Extractor &&) noexcept;
	Extractor &operator=(Extractor &&) noexcept;
	~Extractor();

	explicit Extractor(std::shared_ptr<ytdlpp::net::HttpClient> http,
					   asio::any_io_executor ex);

	[[nodiscard]] asio::any_io_executor get_executor() const;

	// Pre-load the most recent player script from cache to V8.
	// Returns the player_id loaded, or empty string.
	std::string warmup();

	/// Shutdown the extractor, cancelling all active sessions and the JS
	/// engine.
	void shutdown();

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

	/// Search YouTube for videos matching the query.
	/// Returns a list of SearchResult entries.
	template <BOOST_ASIO_COMPLETION_TOKEN_FOR(
		void(Result<std::vector<SearchResult>>)) CompletionToken>
	auto async_search(const SearchOptions &options, CompletionToken &&token) {
		auto ex = get_executor();
		return asio::async_initiate<CompletionToken,
									void(Result<std::vector<SearchResult>>)>(
			[this, ex, options](auto &&handler) mutable {
				CompletionExecutor handler_ex =
					asio::get_associated_executor(handler, ex);

				auto any_handler = asio::any_completion_handler<void(
					Result<std::vector<SearchResult>>)>{
					std::forward<decltype(handler)>(handler)};

				async_search_impl(
					options, std::move(any_handler), std::move(handler_ex));
			},
			token);
	}

   private:
	struct Impl;

	void async_process_impl(
		std::string url,
		asio::any_completion_handler<void(Result<VideoInfo>)> handler,
		CompletionExecutor handler_ex);

	void async_search_impl(
		SearchOptions options,
		asio::any_completion_handler<void(Result<std::vector<SearchResult>>)>
			handler,
		CompletionExecutor handler_ex);

	std::unique_ptr<Impl> m_impl;
};

// JSON Serialization
YTDLPP_EXPORT void to_json(nlohmann::json &j, const VideoFormat &f);
YTDLPP_EXPORT void to_json(nlohmann::json &j, const VideoInfo &i);
YTDLPP_EXPORT void to_json(nlohmann::json &j, const SearchResult &r);

/// Parse a search URL like "ytsearch:query" or "ytsearch5:query"
/// Returns SearchOptions if valid, std::nullopt otherwise
YTDLPP_EXPORT std::optional<SearchOptions> parse_search_url(
	std::string_view url);

}  // namespace ytdlpp::youtube
