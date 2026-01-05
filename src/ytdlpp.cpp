#include <boost/asio.hpp>
#include <iostream>
#include <thread>
#include <utility>
#include <ytdlpp/result.hpp>
#include <ytdlpp/types.hpp>
#include <ytdlpp/ytdlpp.hpp>

#include "downloader/downloader.hpp"
#include "net/http_client.hpp"
#include "scripting/quickjs_engine.hpp"
#include "youtube/extractor.hpp"

namespace ytdlpp {

struct Ytdlpp::Impl {
	boost::asio::any_io_executor ex;
	std::unique_ptr<net::HttpClient> http;
	std::unique_ptr<scripting::JsEngine> js;
	std::unique_ptr<youtube::Extractor> extractor;

	Impl(boost::asio::any_io_executor ex_) : ex(std::move(ex_)) {
		// We need an io_context for HttpClient if it expects one,
		// but here we are given an executor.
		// For now, HttpClient takes a specific io_context& in constructor.
		// This implies we can only support io_context executors properly
		// or we need to untie HttpClient from io_context reference.
		// For this stage, we assume we can't completely refactor internals.
		// We will spin up a dedicated thread/context for internal sync ops
		// if we really wanted to be non-blocking.
		// BUT, for simplicity in this step, let's presume the user passes an
		// executor that we can dispatch to.

		// Actually, HttpClient requires `boost::asio::io_context&`.
		// We'll create a static one for now or own one.
	}

	// Internal dedicated context for blocking ops?
	// Or just shared?
	boost::asio::io_context internal_ioc;

	void setup() {
		http = std::make_unique<net::HttpClient>(internal_ioc);
		js = std::make_unique<scripting::JsEngine>();
		extractor = std::make_unique<youtube::Extractor>(*http, *js);
	}
};

Ytdlpp::Ytdlpp(std::shared_ptr<Impl> impl) : pimpl_(std::move(impl)) {}
Ytdlpp::~Ytdlpp() = default;
Ytdlpp::Ytdlpp(Ytdlpp &&) noexcept = default;
Ytdlpp &Ytdlpp::operator=(Ytdlpp &&) noexcept = default;

Ytdlpp Ytdlpp::create(boost::asio::any_io_executor ex) {
	auto impl = std::make_shared<Impl>(ex);
	impl->setup();
	return Ytdlpp(std::move(impl));
}

boost::asio::any_io_executor Ytdlpp::get_executor() const { return pimpl_->ex; }

void Ytdlpp::extract_impl(
	std::string url,
	boost::asio::any_completion_handler<void(Result<ytdlpp::VideoInfo>)>
		handler) {
	// Post to a thread pool or run synchronously if we assume the caller knows
	// it blocks? The user requested an "async interface". Blocking the executor
	// (likely the main loop) is bad. We'll use std::thread for this simple
	// implementation to avoid blocking the `ex`. In a real production lib we'd
	// have a worker pool.

	std::thread([impl = pimpl_, url = std::move(url),
				 handler = std::move(handler)]() mutable {
		auto res = impl->extractor->process(url);

		// Post completion back to the bound executor
		auto ex = impl->ex;
		boost::asio::post(
			ex, [handler = std::move(handler), res = std::move(res)]() mutable {
				handler(std::move(res));
			});
	}).detach();
}

void Ytdlpp::download_impl(
	ytdlpp::VideoInfo info, std::string selector,
	std::optional<std::string> merge_fmt, ProgressCallback progress_cb,
	boost::asio::any_completion_handler<void(Result<void>)> handler) {
	std::thread([impl = pimpl_, info = std::move(info),
				 selector = std::move(selector),
				 merge_fmt = std::move(merge_fmt),
				 progress_cb = std::move(progress_cb),
				 handler = std::move(handler)]() mutable {
		Downloader downloader(*impl->http);
		bool success =
			downloader.download(info, selector, merge_fmt, progress_cb);

		Result<void> res = outcome::success();
		if (!success) { res = outcome::failure(errc::extraction_failed); }

		auto ex = impl->ex;
		boost::asio::post(ex, [handler = std::move(handler), res]() mutable {
			handler(res);
		});
	}).detach();
}

}  // namespace ytdlpp
