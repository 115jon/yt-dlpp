#include "http_client.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <boost/asio/ssl/error.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/certify/extensions.hpp>
#include <boost/certify/https_verification.hpp>
#include <boost/url.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <vector>

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

namespace ytdlpp::net {

HttpClient::HttpClient(asio::io_context &ioc)
	: ioc_(ioc), ssl_ctx_(ssl::context::tlsv12_client) {
	boost::system::error_code ec;
	ssl_ctx_.set_verify_mode(
		ssl::verify_peer | ssl::verify_fail_if_no_peer_cert, ec);
	if (ec) {
		spdlog::error("Failed to set SSL verify mode: {}", ec.message());
	}

	ssl_ctx_.set_default_verify_paths(ec);
	if (ec) {
		spdlog::error(
			"Failed to set default SSL verify paths: {}", ec.message());
	}

	boost::certify::enable_native_https_server_verification(ssl_ctx_);
}

HttpResponse HttpClient::get(
	const std::string &url, const std::map<std::string, std::string> &headers) {
	return perform_request(http::verb::get, url, "", headers);
}

HttpResponse HttpClient::post(
	const std::string &url, const std::string &body,
	const std::map<std::string, std::string> &headers) {
	return perform_request(http::verb::post, url, body, headers);
}

HttpResponse HttpClient::perform_request(
	http::verb method, const std::string &url_str,
	const std::string &body_content,
	const std::map<std::string, std::string> &headers) {
	try {
		// Parse URL
		boost::urls::url_view u(url_str);
		std::string host = u.host();
		std::string port = u.port();
		std::string target = u.path();
		if (u.has_query()) {
			target += "?";
			target += u.query();
		}
		if (target.empty()) target = "/";
		if (port.empty()) port = (u.scheme() == "https") ? "443" : "80";

		// Resolve
		tcp::resolver resolver(ioc_);
		auto const results = resolver.resolve(host, port);

		// SSL Stream
		beast::ssl_stream<beast::tcp_stream> stream(ioc_, ssl_ctx_);

		// Set SNI Hostname via certify
		boost::certify::set_server_hostname(stream, host);

		// Connect
		beast::get_lowest_layer(stream).connect(results);
		beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(30));

		// Handshake
		stream.handshake(ssl::stream_base::client);

		// Request
		http::request<http::string_body> req{method, target, 11};
		req.set(http::field::host, host);
		req.set(http::field::user_agent, "yt-dlpp/1.0");

		for (const auto &[key, value] : headers) { req.set(key, value); }

		if (!body_content.empty()) {
			req.body() = body_content;
			req.prepare_payload();
		}

		// Send
		http::write(stream, req);

		// Receive
		beast::flat_buffer buffer;
		http::response<http::string_body> res;
		http::read(stream, buffer, res);

		// Graceful shutdown
		beast::error_code ec;
		stream.shutdown(ec);
		if (ec == asio::error::eof) { ec = {}; }

		// Convert headers
		std::map<std::string, std::string> res_headers;
		for (auto const &field : res) {
			res_headers[std::string(field.name_string())] =
				std::string(field.value());
		}

		return HttpResponse{
			static_cast<int>(res.result_int()), res.body(), res_headers};

	} catch (std::exception &e) {
		spdlog::error("Request failed: {}", e.what());
		return HttpResponse{0, "", {}};
	}
}

bool HttpClient::download_file(
	const std::string &url_str, const std::string &output_path,
	std::function<void(long long, long long)> progress_cb) {
	try {
		// Case-insensitive helper
		auto iequals = [](const std::string &a, const std::string &b) {
			if (a.size() != b.size()) return false;
			for (size_t i = 0; i < a.size(); ++i)
				if (tolower(a[i]) != tolower(b[i])) return false;
			return true;
		};

		// 1. Get Content-Length via HEAD (Manual to avoid body limit issues)
		auto get_head_size = [&]() -> long long {
			try {
				boost::urls::url_view u(url_str);
				std::string host = u.host();
				std::string port = u.port();
				std::string target = u.path();
				if (u.has_query()) {
					target += "?";
					target += u.query();
				}
				if (target.empty()) target = "/";
				if (port.empty()) port = (u.scheme() == "https") ? "443" : "80";

				tcp::resolver resolver(ioc_);
				auto const results = resolver.resolve(host, port);
				beast::ssl_stream<beast::tcp_stream> stream(ioc_, ssl_ctx_);
				boost::certify::set_server_hostname(stream, host);
				beast::get_lowest_layer(stream).connect(results);
				beast::get_lowest_layer(stream).expires_after(
					std::chrono::seconds(10));
				stream.handshake(ssl::stream_base::client);

				http::request<http::empty_body> req{
					http::verb::head, target, 11};
				req.set(http::field::host, host);
				req.set(
					http::field::user_agent,
					"Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
					"AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 "
					"Safari/537.36");
				http::write(stream, req);

				http::response_parser<http::empty_body> parser;
				parser.skip(true);
				beast::flat_buffer buffer;
				http::read(stream, buffer, parser);

				beast::error_code ec;
				stream.shutdown(ec);

				if (parser.get().result_int() != 200) return -1;
				auto cl = parser.get().find(http::field::content_length);
				if (cl != parser.get().end())
					return std::stoll(std::string(cl->value()));
				return -1;
			} catch (...) { return -1; }
		};

		long long total_size = get_head_size();

		std::ofstream outfile(output_path, std::ios::binary);
		if (!outfile.is_open()) {
			spdlog::error("Failed to open output file: {}", output_path);
			return false;
		}

		long long current_offset = 0;
		long long chunk_size = 10 * 1024 * 1024;  // 10MB chunks

		// If unknown size, just try one big generic request (old behavior)
		total_size = std::max<long long>(total_size, 0);

		while (true) {
			// Calculate Range
			long long end_range = -1;
			if (total_size > 0) {
				if (current_offset >= total_size) break;  // Done
				end_range =
					std::min(current_offset + chunk_size - 1, total_size - 1);
			}

			// --- Connection & Request Logic (Scoped per chunk) ---
			{
				boost::urls::url_view u(url_str);
				std::string host = u.host();
				std::string port = u.port();
				std::string target = u.path();
				if (u.has_query()) {
					target += "?";
					target += u.query();
				}
				if (target.empty()) target = "/";
				if (port.empty()) port = (u.scheme() == "https") ? "443" : "80";

				tcp::resolver resolver(ioc_);
				auto const results = resolver.resolve(host, port);

				beast::ssl_stream<beast::tcp_stream> stream(ioc_, ssl_ctx_);
				boost::certify::set_server_hostname(stream, host);
				beast::get_lowest_layer(stream).connect(results);
				beast::get_lowest_layer(stream).expires_after(
					std::chrono::seconds(30));
				stream.handshake(ssl::stream_base::client);

				http::request<http::empty_body> req{
					http::verb::get, target, 11};
				req.set(http::field::host, host);
				req.set(
					http::field::user_agent,
					"Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
					"AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 "
					"Safari/537.36");
				req.set(http::field::accept, "*/*");

				// Add Range Header if using chunks
				if (total_size > 0 && end_range >= 0) {
					req.set(http::field::range,
							"bytes=" + std::to_string(current_offset) + "-" +
								std::to_string(end_range));
				}

				http::write(stream, req);

				http::response_parser<http::buffer_body> parser;
				parser.body_limit(boost::none);

				beast::flat_buffer buffer;
				http::read_header(stream, buffer, parser);

				int status = parser.get().result_int();
				if (status != 200 && status != 206) {
					spdlog::error("Download failed. Status: {}", status);
					return false;
				}

				// If we asked for range but got 200, update total_size and
				// assume we have full file
				if (total_size > 0 && status == 200) {
					current_offset = 0;
					outfile.seekp(0);
					end_range = -1;	 // Stop chunking
				}

				// Update total_size if we didn't have it (Single GET case)
				if (total_size <= 0) {
					auto cl_it = parser.get().find(http::field::content_length);
					if (cl_it != parser.get().end()) {
						total_size = std::stoll(std::string(cl_it->value()));
					}
				}

				std::vector<char> buf(1024 * 1024);

				while (!parser.is_done()) {
					parser.get().body().data = buf.data();
					parser.get().body().size = buf.size();

					beast::error_code ec;
					http::read(stream, buffer, parser, ec);

					if (ec == http::error::need_buffer)
						ec = {};
					else if (ec)
						throw beast::system_error{ec};

					size_t bytes_read = buf.size() - parser.get().body().size;
					if (bytes_read > 0) {
						outfile.write(buf.data(), bytes_read);
						current_offset += bytes_read;  // Global tracking

						if (progress_cb)
							progress_cb(current_offset,
										total_size > 0 ? total_size : 0);
					}
				}

				beast::error_code ec;
				stream.shutdown(ec);
			}

			// Break if we just downloaded the whole thing (no chunking active)
			if (total_size <= 0 || end_range == -1) break;
		}

		return true;

	} catch (std::exception &e) {
		spdlog::error("Download request failed: {}", e.what());
		return false;
	}
}

}  // namespace ytdlpp::net
