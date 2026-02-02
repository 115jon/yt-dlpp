#include <spdlog/spdlog.h>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/url.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ytdlpp/cookie_jar.hpp>
#include <ytdlpp/http_client.hpp>

#include "../utils.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

namespace ytdlpp::net {

// ==============================================================================
// HttpClient Implementation
// ==============================================================================

struct HttpClient::Impl {
	asio::any_io_executor ex;
	ssl::context ssl_ctx;
	CookieJar cookie_jar_;

	Impl(asio::any_io_executor ex)
		: ex(std::move(ex)), ssl_ctx(ssl::context::tlsv12_client) {
		ssl_ctx.set_default_verify_paths();
		ssl_ctx.set_verify_mode(ssl::verify_none);	// Simplify for now
	}
};

HttpClient::HttpClient(asio::any_io_executor ex)
	: m_impl(std::make_unique<Impl>(std::move(ex))) {}

HttpClient::~HttpClient() = default;

CookieJar &HttpClient::get_cookie_jar() { return m_impl->cookie_jar_; }

asio::any_io_executor HttpClient::get_executor() const { return m_impl->ex; }

void HttpClient::shutdown() {}

// ------------------------------------------------------------------------------
// RequestSession Base Class
// ------------------------------------------------------------------------------

class RequestSession : public std::enable_shared_from_this<RequestSession> {
   protected:
	asio::strand<asio::any_io_executor> strand_;
	ssl::context &ctx_;
	std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> stream_;
	tcp::resolver resolver_;
	beast::flat_buffer buffer_;
	http::request<http::string_body> req_;
	http::response<http::string_body> res_;
	boost::urls::url url_;

	std::string host_;
	std::string port_;
	std::string path_;

	CookieJar &cookie_jar_;
	asio::any_completion_handler<void(Result<HttpResponse>)> cb_;
	HttpClient::CompletionExecutor handler_ex_;

   public:
	RequestSession(const asio::any_io_executor &ex, ssl::context &ctx,
				   CookieJar &jar,
				   asio::any_completion_handler<void(Result<HttpResponse>)> cb,
				   HttpClient::CompletionExecutor handler_ex)
		: strand_(asio::make_strand(ex)),
		  ctx_(ctx),
		  cookie_jar_(jar),
		  cb_(std::move(cb)),
		  handler_ex_(std::move(handler_ex)),
		  resolver_(strand_) {}

	virtual ~RequestSession() = default;

	void run(const std::string &url_str, const std::string &method,
			 const std::string &body = "",
			 const std::map<std::string, std::string> &headers = {}) {
		auto u_res = boost::urls::parse_uri(url_str);
		if (u_res.has_error()) {
			return fail(make_error_code(errc::invalid_url), "parse_uri");
		}
		url_ = u_res.value();

		host_ = url_.host();
		port_ = url_.has_port() ? url_.port()
								: (url_.scheme() == "https" ? "443" : "80");
		path_ = url_.encoded_resource();

		// Prepare request
		req_.version(11);
		req_.method_string(method);
		req_.target(path_);
		req_.set(http::field::host, host_);
		req_.set(http::field::user_agent, "yt-dlpp/1.0");
		req_.set(http::field::accept, "*/*");

		// Header overrides
		for (const auto &[k, v] : headers) { req_.set(k, v); }

		// Add cookies
		std::string cookie_header = cookie_jar_.build_cookie_header();
		if (!cookie_header.empty()) {
			req_.set(http::field::cookie, cookie_header);
		}

		if (!body.empty()) {
			req_.body() = body;
			req_.prepare_payload();
		}

		start_resolve();
	}

   protected:
	void start_resolve() {
		resolver_.async_resolve(
			host_, port_,
			beast::bind_front_handler(
				&RequestSession::on_resolve, shared_from_this()));
	}

	void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
		if (ec) return fail(ec, "resolve");

		stream_ = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(
			strand_, ctx_);

		beast::get_lowest_layer(*stream_).async_connect(
			results, beast::bind_front_handler(
						 &RequestSession::on_connect, shared_from_this()));
	}

	void on_connect(beast::error_code ec,
					tcp::resolver::endpoint_type /*endpoint*/) {
		if (ec) return fail(ec, "connect");

		stream_->async_handshake(
			ssl::stream_base::client,
			beast::bind_front_handler(
				&RequestSession::on_handshake, shared_from_this()));
	}

	void on_handshake(beast::error_code ec) {
		if (ec) return fail(ec, "handshake");

		http::async_write(*stream_, req_,
						  beast::bind_front_handler(
							  &RequestSession::on_write, shared_from_this()));
	}

	void on_write(beast::error_code ec, std::size_t /*bytes_transferred*/) {
		if (ec) return fail(ec, "write");

		http::async_read(*stream_, buffer_, res_,
						 beast::bind_front_handler(
							 &RequestSession::on_read, shared_from_this()));
	}

	void on_read(beast::error_code ec, std::size_t /*bytes_transferred*/) {
		if (ec) return fail(ec, "read");

		// Handle cookies
		for (auto const &field : res_.base()) {
			if (field.name() == http::field::set_cookie) {
				cookie_jar_.parse_set_cookie(std::string(field.value()));
			}
		}

		HttpResponse out;
		out.status_code = res_.result_int();
		out.body = std::move(res_.body());
		for (const auto &f : res_) {
			out.headers[std::string(f.name_string())] = std::string(f.value());
		}

		post_result(std::move(out));
	}

	void fail(beast::error_code ec, const char *what) {
		spdlog::error("HttpClient error in {}: {}", what, ec.message());
		post_result(outcome::failure(make_error_code(errc::request_failed)));
	}

	void post_result(Result<HttpResponse> res) {
		asio::dispatch(
			handler_ex_, [cb = std::move(cb_), res = std::move(res)]() mutable {
				cb(std::move(res));
			});
	}
};

// ------------------------------------------------------------------------------
// AsyncDownloadSession Implementation
// ------------------------------------------------------------------------------

class AsyncDownloadSession
	: public std::enable_shared_from_this<AsyncDownloadSession> {
	asio::strand<asio::any_io_executor> strand_;
	ssl::context &ctx_;
	CookieJar &cookie_jar_;
	asio::any_completion_handler<void(Result<void>)> cb_;
	HttpClient::CompletionExecutor handler_ex_;
	std::function<void(long long, long long)> progress_cb_;
	std::map<std::string, std::string> headers_;

	std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> stream_;
	tcp::resolver resolver_;
	beast::flat_buffer buffer_;
	http::request<http::empty_body> req_;
	std::unique_ptr<http::response_parser<http::string_body>> parser_;

	boost::urls::url url_;
	std::string host_;
	std::string port_;
	std::string path_;

	std::ofstream outfile_;
	long long total_size_ = -1;
	long long current_offset_ = 0;
	bool head_phase_ = true;

   public:
	static constexpr long long kChunkSize = 2 * 1024 * 1024;

	AsyncDownloadSession(const asio::any_io_executor &ex, ssl::context &ctx,
						 CookieJar &jar,
						 asio::any_completion_handler<void(Result<void>)> cb,
						 HttpClient::CompletionExecutor handler_ex,
						 std::function<void(long long, long long)> progress_cb,
						 std::map<std::string, std::string> headers)
		: strand_(asio::make_strand(ex)),
		  ctx_(ctx),
		  cookie_jar_(jar),
		  cb_(std::move(cb)),
		  handler_ex_(std::move(handler_ex)),
		  progress_cb_(std::move(progress_cb)),
		  headers_(std::move(headers)),
		  resolver_(strand_) {}

	void run(const std::string &url_str, const std::string &output_path) {
		std::filesystem::path p(output_path);
		if (p.has_parent_path()) {
			std::error_code ec;
			std::filesystem::create_directories(p.parent_path(), ec);
		}

		outfile_.open(output_path, std::ios::binary | std::ios::out);
		if (!outfile_.is_open()) {
			return post_result(outcome::failure(errc::file_open_failed));
		}

		auto u_res = boost::urls::parse_uri(url_str);
		if (u_res.has_error())
			return post_result(outcome::failure(errc::invalid_url));
		url_ = u_res.value();

		host_ = url_.host();
		port_ = url_.has_port() ? url_.port()
								: (url_.scheme() == "https" ? "443" : "80");
		path_ = url_.encoded_resource();

		start_resolve();
	}

   private:
	void start_resolve() {
		resolver_.async_resolve(
			host_, port_,
			beast::bind_front_handler(
				&AsyncDownloadSession::on_resolve, shared_from_this()));
	}

	void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
		if (ec) return fail(ec, "resolve");

		stream_ = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(
			strand_, ctx_);

		beast::get_lowest_layer(*stream_).async_connect(
			results,
			beast::bind_front_handler(
				&AsyncDownloadSession::on_connect, shared_from_this()));
	}

	void on_connect(beast::error_code ec,
					tcp::resolver::endpoint_type /*endpoint*/) {
		if (ec) return fail(ec, "connect");

		stream_->async_handshake(
			ssl::stream_base::client,
			beast::bind_front_handler(
				&AsyncDownloadSession::on_handshake, shared_from_this()));
	}

	void on_handshake(beast::error_code ec) {
		if (ec) return fail(ec, "handshake");
		do_write();
	}

	void do_write() {
		req_ = {};
		req_.version(11);
		req_.method(head_phase_ ? http::verb::head : http::verb::get);
		req_.target(path_);
		req_.set(http::field::host, host_);
		req_.set(http::field::user_agent, "yt-dlpp/1.0");
		req_.set(http::field::accept, "*/*");

		for (const auto &[k, v] : headers_) { req_.set(k, v); }

		std::string cookie_header = cookie_jar_.build_cookie_header();
		if (!cookie_header.empty()) {
			req_.set(http::field::cookie, cookie_header);
		}

		if (!head_phase_) {
			long long end = current_offset_ + kChunkSize - 1;
			if (total_size_ > 0 && end >= total_size_) end = total_size_ - 1;

			req_.set(http::field::range,
					 "bytes=" + std::to_string(current_offset_) + "-" +
						 std::to_string(end));
		}

		spdlog::debug("AsyncDownloadSession: Sending {} request to {}",
					  req_.method_string(), path_.substr(0, 100));
		for (const auto &field : req_) {
			spdlog::debug("  {}: {}", field.name_string(), field.value());
		}

		http::async_write(
			*stream_, req_,
			beast::bind_front_handler(
				&AsyncDownloadSession::on_write, shared_from_this()));
	}

	void on_write(beast::error_code ec, std::size_t /*bytes_transferred*/) {
		if (ec) return fail(ec, "write");

		parser_ = std::make_unique<http::response_parser<http::string_body>>();
		parser_->eager(false);

		http::async_read_header(
			*stream_, buffer_, *parser_,
			beast::bind_front_handler(
				&AsyncDownloadSession::on_read_header, shared_from_this()));
	}

	void on_read_header(beast::error_code ec,
						std::size_t /*bytes_transferred*/) {
		if (ec) return fail(ec, "read_header");

		int status = parser_->get().result_int();

		if (status == 200 || status == 206) {
			if (head_phase_) {
				auto cl_it = parser_->get().find(http::field::content_length);
				if (cl_it != parser_->get().end()) {
					total_size_ = utils::to_number_default<long long>(
						std::string(cl_it->value()));
				}

				head_phase_ = false;
				stream_->async_shutdown(beast::bind_front_handler(
					&AsyncDownloadSession::on_shutdown, shared_from_this()));
			} else {
				read_body();
			}
		} else {
			spdlog::warn("Async Download failed status: {}", status);
			return post_result(outcome::failure(errc::request_failed));
		}
	}

	void read_body() {
		http::async_read(
			*stream_, buffer_, *parser_,
			[this, self = shared_from_this()](beast::error_code ec, size_t n) {
				if (ec && ec != http::error::end_of_stream)
					return fail(ec, "read_body");

				const auto &body = parser_->get().body();
				outfile_.write(body.data(), body.size());
				current_offset_ += body.size();

				if (progress_cb_) progress_cb_(current_offset_, total_size_);

				if (total_size_ > 0 && current_offset_ >= total_size_) {
					on_finish(ec);
				} else {
					stream_->async_shutdown(beast::bind_front_handler(
						&AsyncDownloadSession::on_shutdown,
						shared_from_this()));
				}
			});
	}

	void on_shutdown(beast::error_code /*ec*/) {
		if (!head_phase_ &&
			(total_size_ < 0 || current_offset_ < total_size_)) {
			start_resolve();
		} else if (!head_phase_) {
			on_finish({});
		} else {
			start_resolve();
		}
	}

	void on_finish(beast::error_code) {
		outfile_.close();
		post_result(outcome::success());
	}

	void fail(beast::error_code ec, const char *what) {
		spdlog::error(
			"AsyncDownloadSession error in {}: {}", what, ec.message());
		outfile_.close();
		post_result(outcome::failure(make_error_code(errc::request_failed)));
	}

	void post_result(Result<void> res) {
		asio::dispatch(
			handler_ex_, [cb = std::move(cb_), res]() mutable { cb(res); });
	}
};

// ------------------------------------------------------------------------------
// HttpClient Methods
// ------------------------------------------------------------------------------

void HttpClient::async_get_impl(
	std::string url, std::map<std::string, std::string> headers,
	asio::any_completion_handler<void(Result<HttpResponse>)> handler,
	CompletionExecutor handler_ex) {
	std::make_shared<RequestSession>(
		m_impl->ex, m_impl->ssl_ctx, m_impl->cookie_jar_, std::move(handler),
		std::move(handler_ex))
		->run(url, "GET", "", headers);
}

void HttpClient::async_post_impl(
	std::string url, std::string body,
	std::map<std::string, std::string> headers,
	asio::any_completion_handler<void(Result<HttpResponse>)> handler,
	CompletionExecutor handler_ex) {
	std::make_shared<RequestSession>(
		m_impl->ex, m_impl->ssl_ctx, m_impl->cookie_jar_, std::move(handler),
		std::move(handler_ex))
		->run(url, "POST", body, headers);
}

void HttpClient::async_download_file_impl(
	std::string url, std::string output_path, ProgressCallback progress_cb,
	asio::any_completion_handler<void(Result<void>)> handler,
	CompletionExecutor handler_ex, std::map<std::string, std::string> headers) {
	spdlog::debug("async_download_file_impl with {} headers", headers.size());
	for (const auto &[k, v] : headers) {
		spdlog::debug("  Incoming: {}: {}", k, v);
	}

	std::make_shared<AsyncDownloadSession>(
		m_impl->ex, m_impl->ssl_ctx, m_impl->cookie_jar_, std::move(handler),
		std::move(handler_ex), std::move(progress_cb), std::move(headers))
		->run(url, output_path);
}

}  // namespace ytdlpp::net
