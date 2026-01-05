#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <map>
#include <string>

#include "error.hpp"

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace ytdlpp::net {

struct HttpResponse {
	int status_code;
	std::string body;
	std::map<std::string, std::string> headers;
};

class HttpClient {
   public:
	explicit HttpClient(boost::asio::io_context &ioc);

	Result<HttpResponse> get(
		const std::string &url,
		const std::map<std::string, std::string> &headers = {});
	Result<HttpResponse> post(
		const std::string &url, const std::string &body,
		const std::map<std::string, std::string> &headers = {});

	Result<void> download_file(
		const std::string &url, const std::string &output_path,
		std::function<void(long long dl_now, long long dl_total)> progress_cb =
			nullptr);

   private:
	boost::asio::io_context &ioc_;
	ssl::context ssl_ctx_;

	Result<HttpResponse> perform_request(
		http::verb method, const std::string &url, const std::string &body,
		const std::map<std::string, std::string> &headers);
};

}  // namespace ytdlpp::net
