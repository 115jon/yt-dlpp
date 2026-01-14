#include <spdlog/spdlog.h>
#include <zlib.h>

#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core/bind_handler.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/certify/extensions.hpp>
#include <boost/certify/https_verification.hpp>
#include <boost/url.hpp>
#include <chrono>
#include <fstream>
#include <ytdlpp/http_client.hpp>

#include "utils.hpp"

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

namespace ytdlpp::net {

// =============================================================================
// GZIP/DEFLATE DECOMPRESSION
// =============================================================================
// Decompresses gzip or deflate encoded HTTP response bodies.
// This reduces bandwidth usage by ~50% for text-based responses.
// =============================================================================

namespace {

// Decompress gzip or deflate data
std::optional<std::string> decompress_gzip(const std::string &compressed) {
	if (compressed.empty()) return std::string{};

	z_stream zs{};
	// 16 + MAX_WBITS enables gzip decoding
	if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) {
		spdlog::warn("Failed to init zlib for gzip decompression");
		return std::nullopt;
	}

	zs.next_in =
		reinterpret_cast<Bytef *>(const_cast<char *>(compressed.data()));
	zs.avail_in = static_cast<uInt>(compressed.size());

	std::string decompressed;
	decompressed.reserve(compressed.size() * 4);  // Estimate 4x compression

	constexpr size_t kChunkSize = 32768;
	char outbuffer[kChunkSize];

	int ret;
	do {
		zs.next_out = reinterpret_cast<Bytef *>(outbuffer);
		zs.avail_out = kChunkSize;

		ret = inflate(&zs, Z_NO_FLUSH);

		if (ret == Z_NEED_DICT || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
			inflateEnd(&zs);
			spdlog::warn("zlib inflate error: {}", ret);
			return std::nullopt;
		}

		size_t have = kChunkSize - zs.avail_out;
		decompressed.append(outbuffer, have);
	} while (ret != Z_STREAM_END);

	inflateEnd(&zs);
	return decompressed;
}

// Decompress raw deflate data (no gzip header)
std::optional<std::string> decompress_deflate(const std::string &compressed) {
	if (compressed.empty()) return std::string{};

	z_stream zs{};
	// -MAX_WBITS for raw deflate (no header)
	if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
		spdlog::warn("Failed to init zlib for deflate decompression");
		return std::nullopt;
	}

	zs.next_in =
		reinterpret_cast<Bytef *>(const_cast<char *>(compressed.data()));
	zs.avail_in = static_cast<uInt>(compressed.size());

	std::string decompressed;
	decompressed.reserve(compressed.size() * 4);

	constexpr size_t kChunkSize = 32768;
	char outbuffer[kChunkSize];

	int ret;
	do {
		zs.next_out = reinterpret_cast<Bytef *>(outbuffer);
		zs.avail_out = kChunkSize;

		ret = inflate(&zs, Z_NO_FLUSH);

		if (ret == Z_NEED_DICT || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
			inflateEnd(&zs);
			spdlog::warn("zlib deflate error: {}", ret);
			return std::nullopt;
		}

		size_t have = kChunkSize - zs.avail_out;
		decompressed.append(outbuffer, have);
	} while (ret != Z_STREAM_END);

	inflateEnd(&zs);
	return decompressed;
}

// Decompress based on Content-Encoding header
std::string decompress_body(const std::string &body,
							const std::string &content_encoding) {
	if (content_encoding.empty() || content_encoding == "identity") {
		return body;
	}

	if (content_encoding == "gzip" || content_encoding == "x-gzip") {
		if (auto result = decompress_gzip(body)) {
			spdlog::debug("Decompressed gzip: {} -> {} bytes", body.size(),
						  result->size());
			return *result;
		}
		spdlog::warn("gzip decompression failed, returning raw body");
		return body;
	}

	if (content_encoding == "deflate") {
		// Try gzip first (some servers send gzip as deflate)
		if (auto result = decompress_gzip(body)) {
			spdlog::debug("Decompressed deflate (gzip): {} -> {} bytes",
						  body.size(), result->size());
			return *result;
		}
		// Fall back to raw deflate
		if (auto result = decompress_deflate(body)) {
			spdlog::debug("Decompressed deflate: {} -> {} bytes", body.size(),
						  result->size());
			return *result;
		}
		spdlog::warn("deflate decompression failed, returning raw body");
		return body;
	}

	spdlog::debug(
		"Unknown Content-Encoding: {}, returning raw body", content_encoding);
	return body;
}

}  // namespace

// Connection pool entry
struct PooledConnection {
	std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> stream;
	std::chrono::steady_clock::time_point last_used;
};

// =============================================================================
// DNS CACHE
// =============================================================================
// Caches DNS lookup results to avoid repeated resolution for the same hosts.
// This saves ~50-150ms per new connection to the same host.
// Common hosts: youtube.com, www.youtube.com, *.googlevideo.com
// =============================================================================

struct DnsCacheEntry {
	tcp::resolver::results_type results;
	std::chrono::steady_clock::time_point expires_at;
};

class DnsCache {
   public:
	static constexpr auto kDefaultTTL = std::chrono::minutes(5);
	static constexpr size_t kMaxCacheSize = 64;

	// Get cached results or nullopt if not found/expired
	std::optional<tcp::resolver::results_type> get(const std::string &host,
												   const std::string &port) {
		std::lock_guard lock(mutex_);
		auto key = host + ":" + port;
		auto it = cache_.find(key);
		if (it == cache_.end()) { return std::nullopt; }
		if (std::chrono::steady_clock::now() > it->second.expires_at) {
			cache_.erase(it);
			return std::nullopt;
		}
		spdlog::debug("DNS cache hit for {}", key);
		return it->second.results;
	}

	// Store results in cache
	void put(const std::string &host, const std::string &port,
			 const tcp::resolver::results_type &results,
			 std::chrono::steady_clock::duration ttl = kDefaultTTL) {
		std::lock_guard lock(mutex_);
		auto key = host + ":" + port;

		// Evict expired entries if cache is full
		if (cache_.size() >= kMaxCacheSize) { evict_expired(); }

		// If still full, evict oldest entry
		if (cache_.size() >= kMaxCacheSize) {
			auto oldest = cache_.begin();
			for (auto it = cache_.begin(); it != cache_.end(); ++it) {
				if (it->second.expires_at < oldest->second.expires_at) {
					oldest = it;
				}
			}
			cache_.erase(oldest);
		}

		cache_[key] =
			DnsCacheEntry{results, std::chrono::steady_clock::now() + ttl};
		spdlog::debug(
			"DNS cached {} ({} results, TTL {}s)", key,
			std::distance(results.begin(), results.end()),
			std::chrono::duration_cast<std::chrono::seconds>(ttl).count());
	}

	// Clear all cached entries
	void clear() {
		std::lock_guard lock(mutex_);
		cache_.clear();
	}

	// Invalidate a specific host (useful on connection errors)
	void invalidate(const std::string &host, const std::string &port) {
		std::lock_guard lock(mutex_);
		cache_.erase(host + ":" + port);
	}

   private:
	void evict_expired() {
		auto now = std::chrono::steady_clock::now();
		for (auto it = cache_.begin(); it != cache_.end();) {
			if (now > it->second.expires_at) {
				it = cache_.erase(it);
			} else {
				++it;
			}
		}
	}

	std::mutex mutex_;
	std::unordered_map<std::string, DnsCacheEntry> cache_;
};

// Global DNS cache (shared across all HttpClient instances)
static DnsCache &get_dns_cache() {
	static DnsCache instance;
	return instance;
}

// Forward declaration for session cancellation interface
class IActiveSession {
   public:
	virtual ~IActiveSession() = default;
	virtual void cancel() = 0;
};

struct HttpClient::Impl {
	asio::any_io_executor ex;
	ssl::context ssl_ctx;

	// Connection pool: host:port -> list of connections
	std::mutex pool_mutex_;
	std::unordered_map<std::string, std::vector<PooledConnection>> conn_pool_;
	static constexpr auto kConnectionTimeout = std::chrono::seconds(30);
	static constexpr size_t kMaxPoolSize = 4;

	// Active session tracking for cancellation
	std::mutex sessions_mutex_;
	std::vector<std::weak_ptr<IActiveSession>> active_sessions_;
	std::atomic<bool> shutdown_requested_{false};

	Impl(asio::any_io_executor e)
		: ex(std::move(e)), ssl_ctx(ssl::context::tlsv12_client) {
		boost::system::error_code ec;
		ssl_ctx.set_verify_mode(
			ssl::verify_peer | ssl::verify_fail_if_no_peer_cert, ec);
		if (ec) {
			spdlog::error("Failed to set SSL verify mode: {}", ec.message());
		}

		ssl_ctx.set_default_verify_paths(ec);
		if (ec) {
			spdlog::error(
				"Failed to set default SSL verify paths: {}", ec.message());
		}

		boost::certify::enable_native_https_server_verification(ssl_ctx);
	}

	void register_session(std::weak_ptr<IActiveSession> session) {
		std::lock_guard lock(sessions_mutex_);
		// Cleanup expired sessions while we're here
		active_sessions_.erase(
			std::remove_if(active_sessions_.begin(), active_sessions_.end(),
						   [](const auto &wp) { return wp.expired(); }),
			active_sessions_.end());
		active_sessions_.push_back(std::move(session));
	}

	void shutdown() {
		shutdown_requested_.store(true, std::memory_order_release);

		// Cancel all active sessions
		{
			std::lock_guard lock(sessions_mutex_);
			for (auto &wp : active_sessions_) {
				if (auto sp = wp.lock()) { sp->cancel(); }
			}
			active_sessions_.clear();
		}

		// Close all pooled connections
		{
			std::lock_guard lock(pool_mutex_);
			for (auto &[key, conns] : conn_pool_) {
				for (auto &conn : conns) {
					if (conn.stream) {
						beast::get_lowest_layer(*conn.stream).cancel();
					}
				}
			}
			conn_pool_.clear();
		}
	}

	[[nodiscard]] bool is_shutdown() const {
		return shutdown_requested_.load(std::memory_order_acquire);
	}

	// Get a pooled connection or nullptr if none available
	std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> acquire_connection(
		const std::string &host, const std::string &port) {
		std::lock_guard lock(pool_mutex_);
		std::string key = host + ":" + port;
		auto it = conn_pool_.find(key);
		if (it != conn_pool_.end() && !it->second.empty()) {
			auto now = std::chrono::steady_clock::now();
			// Find a non-stale connection
			while (!it->second.empty()) {
				auto &conn = it->second.back();
				if (now - conn.last_used < kConnectionTimeout) {
					auto stream = std::move(conn.stream);
					it->second.pop_back();
					spdlog::debug("Reusing pooled connection for {}", key);
					return stream;
				}
				// Stale, discard
				it->second.pop_back();
			}
		}
		return nullptr;
	}

	// Return a connection to the pool
	void release_connection(
		const std::string &host, const std::string &port,
		std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> stream) {
		if (!stream) return;
		std::lock_guard lock(pool_mutex_);
		std::string key = host + ":" + port;
		auto &conns = conn_pool_[key];
		if (conns.size() < kMaxPoolSize) {
			conns.push_back(
				{std::move(stream), std::chrono::steady_clock::now()});
			spdlog::debug("Returned connection to pool for {} (pool size: {})",
						  key, conns.size());
		}
		// If pool is full, just let the stream destruct
	}

	Result<HttpResponse> perform_request(
		http::verb method, const std::string &url_str,
		const std::string &body_content,
		const std::map<std::string, std::string> &headers) {
		try {
			// Parse URL
			auto u_res = boost::urls::parse_uri(url_str);
			if (u_res.has_error()) return outcome::failure(errc::invalid_url);

			boost::urls::url_view u = u_res.value();

			std::string host = u.host();
			std::string port = u.port();
			std::string target = u.path();
			if (u.has_query()) {
				target += "?";
				target += u.query();
			}
			if (target.empty()) target = "/";
			if (port.empty()) port = (u.scheme() == "https") ? "443" : "80";

			// Try to get a pooled connection
			auto pooled_stream = acquire_connection(host, port);
			std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> stream_ptr;

			if (pooled_stream) {
				stream_ptr = std::move(pooled_stream);
			} else {
				// Create new connection
				boost::system::error_code ec;
				tcp::resolver resolver(ex);

				// Check DNS cache first
				tcp::resolver::results_type results;
				auto cached = get_dns_cache().get(host, port);
				if (cached) {
					results = *cached;
				} else {
					results = resolver.resolve(host, port, ec);
					if (ec)
						return outcome::failure(
							make_error_code(errc::request_failed));
					// Cache the results
					get_dns_cache().put(host, port, results);
				}

				stream_ptr =
					std::make_unique<beast::ssl_stream<beast::tcp_stream>>(
						ex, ssl_ctx);
				boost::certify::set_server_hostname(*stream_ptr, host);

				beast::get_lowest_layer(*stream_ptr).connect(results, ec);
				if (ec)
					return outcome::failure(
						make_error_code(errc::request_failed));

				beast::get_lowest_layer(*stream_ptr)
					.expires_after(std::chrono::seconds(30));

				stream_ptr->handshake(ssl::stream_base::client, ec);
				if (ec)
					return outcome::failure(
						make_error_code(errc::request_failed));
			}

			boost::system::error_code ec;
			beast::ssl_stream<beast::tcp_stream> &stream = *stream_ptr;

			beast::get_lowest_layer(stream).expires_after(
				std::chrono::seconds(30));

			// Request
			http::request<http::string_body> req{method, target, 11};
			req.set(http::field::host, host);
			req.set(http::field::user_agent, "yt-dlpp/1.0");
			req.set(http::field::connection, "keep-alive");
			// Request compressed responses to save bandwidth
			req.set(http::field::accept_encoding, "gzip, deflate");

			for (const auto &[key, value] : headers) { req.set(key, value); }

			if (!body_content.empty()) {
				req.body() = body_content;
				req.prepare_payload();
			}

			// Send
			http::write(stream, req, ec);
			if (ec)
				return outcome::failure(make_error_code(errc::request_failed));

			// Receive
			beast::flat_buffer buffer;
			http::response<http::string_body> res;
			http::read(stream, buffer, res, ec);
			if (ec)
				return outcome::failure(make_error_code(errc::request_failed));

			// Check keep-alive and return to pool if possible
			if (res.keep_alive()) {
				release_connection(host, port, std::move(stream_ptr));
			} else {
				// Graceful shutdown
				stream.shutdown(ec);
			}

			// Get Content-Encoding header
			std::string content_encoding;
			auto encoding_it = res.find(http::field::content_encoding);
			if (encoding_it != res.end()) {
				content_encoding = std::string(encoding_it->value());
			}

			// Decompress body if needed
			std::string response_body =
				decompress_body(res.body(), content_encoding);

			// Convert headers
			std::map<std::string, std::string> res_headers;
			for (auto const &field : res) {
				res_headers[std::string(field.name_string())] =
					std::string(field.value());
			}

			return HttpResponse{
				static_cast<int>(res.result_int()), response_body, res_headers};

		} catch (const std::exception &e) {
			spdlog::error("Request exception: {}", e.what());
			return outcome::failure(errc::request_failed);
		}
	}
};

HttpClient::HttpClient(asio::any_io_executor ex)
	: m_impl(std::make_unique<Impl>(std::move(ex))) {}

HttpClient::~HttpClient() = default;
HttpClient::HttpClient(HttpClient &&) noexcept = default;
HttpClient &HttpClient::operator=(HttpClient &&) noexcept = default;

asio::any_io_executor HttpClient::get_executor() const { return m_impl->ex; }

void HttpClient::shutdown() {
	if (m_impl) { m_impl->shutdown(); }
}

class RequestSession : public IActiveSession,
					   public std::enable_shared_from_this<RequestSession> {
   public:
	using CompletionExecutor = asio::any_completion_executor;

	RequestSession(HttpClient::Impl *impl, const asio::any_io_executor &ex,
				   ssl::context &ctx,
				   asio::any_completion_handler<void(Result<HttpResponse>)> cb,
				   CompletionExecutor handler_ex)
		: impl_(impl),
		  strand_(asio::make_strand(ex)),
		  resolver_(strand_),
		  stream_(strand_, ctx),
		  cb_(std::move(cb)),
		  handler_ex_(std::move(handler_ex)) {}

	void cancel() override {
		// Cancel resolver and stream operations
		resolver_.cancel();
		beast::get_lowest_layer(stream_).cancel();
	}

	void run(http::verb method, const std::string &url_str,
			 const std::string &body_content,
			 const std::map<std::string, std::string> &headers) {
		// Parse URL
		auto u_res = boost::urls::parse_uri(url_str);
		if (u_res.has_error()) {
			return post_result(outcome::failure(errc::invalid_url));
		}
		boost::urls::url_view u = u_res.value();

		std::string host = u.host();
		std::string port = u.port();
		std::string target = u.path();
		if (u.has_query()) {
			target += "?";
			target += u.query();
		}
		if (target.empty()) target = "/";
		if (port.empty()) port = (u.scheme() == "https") ? "443" : "80";

		req_.version(11);
		req_.method(method);
		req_.target(target);
		req_.set(http::field::host, host);
		req_.set(http::field::user_agent, "yt-dlpp/1.0");
		// Request compressed responses to save bandwidth
		req_.set(http::field::accept_encoding, "gzip, deflate");
		for (const auto &[key, value] : headers) { req_.set(key, value); }
		if (!body_content.empty()) {
			req_.body() = body_content;
			req_.prepare_payload();
		}

		// Set SNI
		if (!SSL_set_tlsext_host_name(stream_.native_handle(), host.c_str())) {
			return post_result(
				outcome::failure(make_error_code(errc::request_failed)));
		}

		// Store host/port for DNS caching
		host_ = host;
		port_ = port;

		// Check DNS cache first
		auto cached = get_dns_cache().get(host, port);
		if (cached) {
			// Use cached results, skip DNS lookup
			on_resolve({}, *cached);
		} else {
			resolver_.async_resolve(
				host, port,
				beast::bind_front_handler(
					&RequestSession::on_resolve, shared_from_this()));
		}
	}

	void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
		if (ec)
			return post_result(
				outcome::failure(make_error_code(errc::request_failed)));

		// Cache the DNS results for future requests
		get_dns_cache().put(host_, port_, results);

		beast::get_lowest_layer(stream_).expires_after(
			std::chrono::seconds(30));
		beast::get_lowest_layer(stream_).async_connect(
			results, beast::bind_front_handler(
						 &RequestSession::on_connect, shared_from_this()));
	}

	void on_connect(beast::error_code ec, tcp::endpoint /*unused*/) {
		if (ec)
			return post_result(
				outcome::failure(make_error_code(errc::request_failed)));

		stream_.async_handshake(
			ssl::stream_base::client,
			beast::bind_front_handler(
				&RequestSession::on_handshake, shared_from_this()));
	}

	void on_handshake(beast::error_code ec) {
		if (ec)
			return post_result(
				outcome::failure(make_error_code(errc::request_failed)));

		http::async_write(stream_, req_,
						  beast::bind_front_handler(
							  &RequestSession::on_write, shared_from_this()));
	}

	void on_write(beast::error_code ec, std::size_t) {
		if (ec)
			return post_result(
				outcome::failure(make_error_code(errc::request_failed)));

		http::async_read(stream_, buf_, res_,
						 beast::bind_front_handler(
							 &RequestSession::on_read, shared_from_this()));
	}

	void on_read(beast::error_code ec, std::size_t) {
		if (ec)
			return post_result(
				outcome::failure(make_error_code(errc::request_failed)));

		// Graceful close - set short timeout
		beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(2));
		stream_.async_shutdown(beast::bind_front_handler(
			&RequestSession::on_shutdown, shared_from_this()));
	}

	void on_shutdown(beast::error_code /*ec*/) {
		// Ignore shutdown errors (eof, timeout, etc) since we have the body

		// Get Content-Encoding header and decompress if needed
		std::string content_encoding;
		auto encoding_it = res_.find(http::field::content_encoding);
		if (encoding_it != res_.end()) {
			content_encoding = std::string(encoding_it->value());
		}
		std::string response_body =
			decompress_body(res_.body(), content_encoding);

		// Convert headers
		std::map<std::string, std::string> res_headers;
		for (auto const &field : res_) {
			res_headers[std::string(field.name_string())] =
				std::string(field.value());
		}

		post_result(HttpResponse{
			static_cast<int>(res_.result_int()), response_body, res_headers});
	}

	void post_result(Result<HttpResponse> res) {
		asio::dispatch(
			handler_ex_, [cb = std::move(cb_), res = std::move(res)]() mutable {
				cb(std::move(res));
			});
	}

   private:
	HttpClient::Impl *impl_;
	asio::strand<asio::any_io_executor> strand_;
	tcp::resolver resolver_;
	beast::ssl_stream<beast::tcp_stream> stream_;
	asio::any_completion_handler<void(Result<HttpResponse>)> cb_;
	CompletionExecutor handler_ex_;
	beast::flat_buffer buf_;
	http::request<http::string_body> req_;
	http::response<http::string_body> res_;
	std::string host_;	// For DNS caching
	std::string port_;	// For DNS caching
};

void HttpClient::async_get_impl(
	std::string url, std::map<std::string, std::string> headers,
	asio::any_completion_handler<void(Result<HttpResponse>)> handler,
	CompletionExecutor handler_ex) {
	auto session = std::make_shared<RequestSession>(
		m_impl.get(), m_impl->ex, m_impl->ssl_ctx, std::move(handler),
		std::move(handler_ex));
	m_impl->register_session(session);
	session->run(http::verb::get, url, "", headers);
}

void HttpClient::async_post_impl(
	std::string url, std::string body,
	std::map<std::string, std::string> headers,
	asio::any_completion_handler<void(Result<HttpResponse>)> handler,
	CompletionExecutor handler_ex) {
	auto session = std::make_shared<RequestSession>(
		m_impl.get(), m_impl->ex, m_impl->ssl_ctx, std::move(handler),
		std::move(handler_ex));
	m_impl->register_session(session);
	session->run(http::verb::post, url, body, headers);
}

class AsyncDownloadSession
	: public std::enable_shared_from_this<AsyncDownloadSession> {
   public:
	using CompletionExecutor = HttpClient::CompletionExecutor;

	// ==========================================================================
	// BUFFER SIZE CONSTANTS (Optimized for low-memory devices like Raspberry
	// Pi)
	// ==========================================================================
	// kChunkSize: Size of HTTP Range request chunks. Smaller = less memory,
	//             but more HTTP requests. 2MB is a good balance.
	// kReadBufferSize: Size of the read buffer for body data. 256KB provides
	//                  good throughput while being memory-efficient.
	// ==========================================================================
	static constexpr long long kChunkSize = 2 * 1024 * 1024;  // 2MB
	static constexpr size_t kReadBufferSize = 256 * 1024;	  // 256KB

	AsyncDownloadSession(const asio::any_io_executor &ex, ssl::context &ctx,
						 asio::any_completion_handler<void(Result<void>)> cb,
						 CompletionExecutor handler_ex,
						 std::function<void(long long, long long)> progress_cb)
		: strand_(asio::make_strand(ex)),
		  ctx_(ctx),
		  cb_(std::move(cb)),
		  handler_ex_(std::move(handler_ex)),
		  progress_cb_(std::move(progress_cb)) {}

	void run(const std::string &url_str, const std::string &output_path) {
		outfile_.open(output_path, std::ios::binary | std::ios::out);
		if (!outfile_.is_open()) {
			return post_result(outcome::failure(errc::file_open_failed));
		}

		auto u_res = boost::urls::parse_uri(url_str);
		if (u_res.has_error())
			return post_result(outcome::failure(errc::invalid_url));
		url_ = u_res.value();

		// Defaults
		host_ = url_.host();
		port_ = url_.port();
		path_ = url_.path();
		if (url_.has_query()) {
			path_ += "?";
			path_ += url_.query();
		}
		if (path_.empty()) path_ = "/";
		if (port_.empty()) port_ = (url_.scheme() == "https") ? "443" : "80";

		// Start with HEAD request
		head_phase_ = true;
		start_resolve();
	}

   private:
	asio::strand<asio::any_io_executor> strand_;
	ssl::context &ctx_;
	std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> stream_;
	std::unique_ptr<tcp::resolver> resolver_;

	asio::any_completion_handler<void(Result<void>)> cb_;
	CompletionExecutor handler_ex_;
	std::function<void(long long, long long)> progress_cb_;

	std::ofstream outfile_;
	boost::urls::url_view url_;
	std::string host_, port_, path_;

	http::request<http::empty_body> req_;
	std::optional<http::response_parser<http::buffer_body>> parser_;
	std::optional<http::response_parser<http::empty_body>> head_parser_;

	beast::flat_buffer buffer_;
	// Read buffer - sized according to kReadBufferSize constant
	std::vector<char> buf_{std::vector<char>(kReadBufferSize)};

	long long total_size_ = -1;
	long long current_offset_ = 0;
	bool head_phase_ = false;

	void start_next_chunk(bool reuse = false) {
		if (total_size_ > 0 && current_offset_ >= total_size_) {
			return on_finish(beast::error_code{});
		}

		if (reuse && stream_) {
			// Reuse existing connection
			head_phase_ = false;
			parser_.emplace();
			parser_->body_limit(boost::none);
			do_write();
		} else {
			head_phase_ = false;
			start_resolve();
		}
	}

	void start_resolve() {
		// Re-create stream and resolver for fresh connection
		stream_ = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(
			strand_, ctx_);
		resolver_ = std::make_unique<tcp::resolver>(strand_);

		if (!head_phase_) {
			parser_.emplace();
			parser_->body_limit(boost::none);
		} else {
			head_parser_.emplace();
			head_parser_->skip(true);
		}

		buffer_.clear();

		if (!SSL_set_tlsext_host_name(
				stream_->native_handle(), host_.c_str())) {
			return post_result(
				outcome::failure(make_error_code(errc::request_failed)));
		}

		// Check DNS cache first
		auto cached = get_dns_cache().get(host_, port_);
		if (cached) {
			// Use cached results, skip DNS lookup
			on_resolve({}, *cached);
		} else {
			resolver_->async_resolve(
				host_, port_,
				beast::bind_front_handler(
					&AsyncDownloadSession::on_resolve, shared_from_this()));
		}
	}

	void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
		if (ec) return fail(ec, "resolve");

		// Cache the DNS results for future requests
		get_dns_cache().put(host_, port_, results);

		beast::get_lowest_layer(*stream_).expires_after(
			std::chrono::seconds(30));
		beast::get_lowest_layer(*stream_).async_connect(
			results,
			beast::bind_front_handler(
				&AsyncDownloadSession::on_connect, shared_from_this()));
	}

	void on_connect(beast::error_code ec, tcp::endpoint) {
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
		// Prepare Request
		req_ = {};
		req_.version(11);
		req_.target(path_);
		req_.set(http::field::host, host_);
		req_.set(http::field::user_agent, "yt-dlpp/1.0");
		req_.set(http::field::accept, "*/*");

		if (head_phase_) {
			req_.method(http::verb::head);
		} else {
			req_.method(http::verb::get);
			if (total_size_ > 0 || current_offset_ == 0) {
				// Calculate range
				long long end = -1;
				if (total_size_ > 0) {
					end = std::min(
						current_offset_ + kChunkSize - 1, total_size_ - 1);
				} else {
					// Try probing with first chunk
					end = kChunkSize - 1;
				}

				std::string range_val =
					"bytes=" + std::to_string(current_offset_) + "-" +
					std::to_string(end);
				req_.set(http::field::range, range_val);
			}
		}

		http::async_write(
			*stream_, req_,
			beast::bind_front_handler(
				&AsyncDownloadSession::on_write, shared_from_this()));
	}

	void on_write(beast::error_code ec, std::size_t) {
		if (ec) return fail(ec, "write");

		if (head_phase_) {
			http::async_read(
				*stream_, buffer_, *head_parser_,
				beast::bind_front_handler(
					&AsyncDownloadSession::on_head_read, shared_from_this()));
		} else {
			http::async_read_header(
				*stream_, buffer_, *parser_,
				beast::bind_front_handler(
					&AsyncDownloadSession::on_read_header, shared_from_this()));
		}
	}

	void on_head_read(beast::error_code ec, std::size_t) {
		// Even if head fails, we proceed to download and hope for the best
		if (!ec) {
			int status = head_parser_->get().result_int();
			if (status == 200) {
				auto cl_it =
					head_parser_->get().find(http::field::content_length);
				if (cl_it != head_parser_->get().end()) {
					auto len_opt = utils::to_long(std::string_view(
						cl_it->value().data(), cl_it->value().size()));
					if (len_opt) total_size_ = len_opt.value();
				}
			}
		} else {
			spdlog::warn("HEAD request failed: {}", ec.message());
		}

		// Check for keep-alive to reuse connection
		bool keep_alive = head_parser_->get().keep_alive();

		if (keep_alive) {
			// Reuse for download
			start_next_chunk(true);
		} else {
			// Shutdown head connection and start download fresh
			stream_->async_shutdown(beast::bind_front_handler(
				&AsyncDownloadSession::on_shutdown, shared_from_this()));
		}
	}

	void on_read_header(beast::error_code ec, std::size_t /*unused*/) {
		if (ec) return fail(ec, "read_header");

		int status = parser_->get().result_int();

		if (status == 200) {
			// Server ignored Range, we are getting full file
			current_offset_ = 0;
			outfile_.seekp(0);
			// Total size might be in Content-Length
			auto cl_it = parser_->get().find(http::field::content_length);
			if (cl_it != parser_->get().end()) {
				auto len_opt = utils::to_long(std::string_view(
					cl_it->value().data(), cl_it->value().size()));
				if (len_opt) total_size_ = len_opt.value();
			} else {
				total_size_ = -1;  // Unknown
			}
		} else if (status == 206) {
			// Partial Content
			auto cr_it = parser_->get().find(http::field::content_range);
			if (cr_it != parser_->get().end()) {
				// Parse "bytes start-end/total"
				std::string_view cr = cr_it->value();
				auto slash_pos = cr.find('/');
				if (slash_pos != std::string_view::npos) {
					std::string_view total_sv = cr.substr(slash_pos + 1);
					auto len_opt = utils::to_long(total_sv);
					if (len_opt) total_size_ = len_opt.value();
				}
			}
		} else {
			spdlog::warn("Async Download failed status: {}", status);
			return post_result(outcome::failure(errc::request_failed));
		}

		read_body();
	}

	void read_body() {
		if (parser_->is_done()) { return on_chunk_finish(); }

		beast::get_lowest_layer(*stream_).expires_after(
			std::chrono::seconds(30));

		parser_->get().body().data = buf_.data();
		parser_->get().body().size = buf_.size();

		http::async_read(
			*stream_, buffer_, *parser_,
			beast::bind_front_handler(
				&AsyncDownloadSession::on_read_body, shared_from_this()));
	}

	void on_read_body(beast::error_code ec, std::size_t) {
		if (ec == http::error::need_buffer) ec = {};
		if (ec) return fail(ec, "read_body");

		size_t bytes_read = buf_.size() - parser_->get().body().size;
		if (bytes_read > 0) {
			outfile_.write(buf_.data(), bytes_read);
			current_offset_ += bytes_read;
			if (progress_cb_)
				progress_cb_(
					current_offset_, total_size_ > 0 ? total_size_ : 0);
		}

		read_body();
	}

	void on_chunk_finish() {
		// Check Keep-Alive
		bool keep_alive = parser_->get().keep_alive();
		if (keep_alive && (total_size_ <= 0 || current_offset_ < total_size_)) {
			// Reuse connection
			return start_next_chunk(true);
		}

		// Graceful shutdown of this connection
		stream_->async_shutdown(beast::bind_front_handler(
			&AsyncDownloadSession::on_shutdown, shared_from_this()));
	}

	void on_shutdown(beast::error_code /*unused*/) {
		if (head_phase_) {
			head_phase_ = false;
			start_resolve();
		} else {
			start_next_chunk(false);
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

void HttpClient::async_download_file_impl(
	std::string url, std::string output_path, ProgressCallback progress_cb,
	asio::any_completion_handler<void(Result<void>)> handler,
	CompletionExecutor handler_ex) {
	std::make_shared<AsyncDownloadSession>(
		m_impl->ex, m_impl->ssl_ctx, std::move(handler), std::move(handler_ex),
		std::move(progress_cb))
		->run(url, output_path);
}

}  // namespace ytdlpp::net
