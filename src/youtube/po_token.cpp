#include "po_token.hpp"

#include <spdlog/spdlog.h>

#include <boost/beast/core/detail/base64.hpp>
#include <boost/regex.hpp>
#include <boost/url/decode_view.hpp>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace ytdlpp::youtube {

// WebPO clients that support PO Tokens
static const std::vector<std::string> WEBPO_CLIENTS = {
	"WEB",
	"MWEB",
	"TVHTML5",
	"WEB_EMBEDDED_PLAYER",
	"WEB_CREATOR",
	"WEB_REMIX",
	"TVHTML5_SIMPLY",
	"TVHTML5_SIMPLY_EMBEDDED_PLAYER"};

// Base64 encoding/decoding using Boost.Beast
namespace beast_base64 = boost::beast::detail::base64;

static std::vector<uint8_t> base64_decode(const std::string &input) {
	std::vector<uint8_t> result;
	result.resize(beast_base64::decoded_size(input.size()));
	auto [out_len, _] =
		beast_base64::decode(result.data(), input.data(), input.size());
	result.resize(out_len);
	return result;
}

static std::string base64_encode(const std::vector<uint8_t> &input) {
	std::string result;
	result.resize(beast_base64::encoded_size(input.size()));
	beast_base64::encode(result.data(), input.data(), input.size());
	return result;
}

// PoTokenProvider base implementation
bool PoTokenProvider::supports_request(const PoTokenRequest &request) const {
	// Check context support
	if (!supported_contexts_.empty()) {
		bool context_supported = false;
		for (auto ctx : supported_contexts_) {
			if (ctx == request.context) {
				context_supported = true;
				break;
			}
		}
		if (!context_supported) return false;
	}

	// Check client support
	if (!supported_clients_.empty()) {
		auto client_name = request.innertube_context.value("clientName", "");
		bool client_supported = false;
		for (const auto &client : supported_clients_) {
			if (client == client_name) {
				client_supported = true;
				break;
			}
		}
		if (!client_supported) return false;
	}

	return true;
}

// WebPoCacheSpecProvider implementation
WebPoCacheSpecProvider::WebPoCacheSpecProvider() {
	// WebPO supports GVS, PLAYER, and SUBS contexts
	supported_contexts_ = {
		PoTokenContext::GVS, PoTokenContext::PLAYER, PoTokenContext::SUBS};
	// WebPO supports all WebPO clients
	supported_clients_ = WEBPO_CLIENTS;
}

std::optional<PoTokenResponse> WebPoCacheSpecProvider::request_pot(
	const PoTokenRequest & /*request*/) {
	// This provider only generates cache specs, not actual tokens
	// The actual token would be fetched from a remote service
	return std::nullopt;
}

std::optional<PoTokenCacheSpec> WebPoCacheSpecProvider::generate_cache_spec(
	const PoTokenRequest &request) const {
	auto [content_binding, binding_type] = get_content_binding(request, true);

	if (content_binding.empty()) { return std::nullopt; }

	PoTokenCacheSpec spec;
	spec.key_bindings["t"] = "webpo";
	spec.key_bindings["cb"] = content_binding;
	spec.key_bindings["cbt"] = [binding_type]() {
		switch (binding_type) {
			case ContentBindingType::VISITOR_DATA: return "visitor_data";
			case ContentBindingType::DATASYNC_ID: return "datasync_id";
			case ContentBindingType::VIDEO_ID: return "video_id";
			case ContentBindingType::VISITOR_ID: return "visitor_id";
		}
		return "unknown";
	}();

	// Add network info if available
	if (request.innertube_context.contains("client") &&
		request.innertube_context["client"].contains("remoteHost")) {
		spec.key_bindings["ip"] =
			request.innertube_context["client"]["remoteHost"]
				.get<std::string>();
	}
	if (!request.request_proxy.empty()) {
		spec.key_bindings["px"] = request.request_proxy;
	}

	return spec;
}

std::pair<std::string, ContentBindingType>
WebPoCacheSpecProvider::get_content_binding(const PoTokenRequest &request,
											bool bind_to_visitor_id) const {
	auto client_name =
		request.innertube_context.value("client", nlohmann::json::object())
			.value("clientName", "");

	// Check if this is a WebPO client
	bool is_webpo = false;
	for (const auto &client : WEBPO_CLIENTS) {
		if (client == client_name) {
			is_webpo = true;
			break;
		}
	}
	if (!is_webpo) { return {"", ContentBindingType::VISITOR_DATA}; }

	// GVS context with video ID binding
	if (request.context == PoTokenContext::GVS &&
		request.gvs_bind_to_video_id) {
		return {request.video_id, ContentBindingType::VIDEO_ID};
	}

	// GVS context or WEB_REMIX - bind to visitor data or data_sync_id
	if (request.context == PoTokenContext::GVS || client_name == "WEB_REMIX") {
		if (request.is_authenticated && !request.data_sync_id.empty()) {
			return {request.data_sync_id, ContentBindingType::DATASYNC_ID};
		} else {
			if (bind_to_visitor_id) {
				auto visitor_id = extract_visitor_id(request.visitor_data);
				if (visitor_id) {
					return {*visitor_id, ContentBindingType::VISITOR_ID};
				}
			}
			return {request.visitor_data, ContentBindingType::VISITOR_DATA};
		}
	}

	// PLAYER or SUBS context - bind to video ID
	if (request.context == PoTokenContext::PLAYER ||
		request.context == PoTokenContext::SUBS) {
		return {request.video_id, ContentBindingType::VIDEO_ID};
	}

	return {"", ContentBindingType::VISITOR_DATA};
}

std::optional<std::string> WebPoCacheSpecProvider::extract_visitor_id(
	const std::string &visitor_data) const {
	if (visitor_data.empty()) { return std::nullopt; }

	try {
		// URL decode using Boost.URL decode_view
		boost::urls::decode_view decoded_view(visitor_data);
		std::string decoded(decoded_view.begin(), decoded_view.end());

		// Base64 decode
		auto bytes = base64_decode(decoded);
		if (bytes.size() < 13) { return std::nullopt; }

		// Extract visitor ID (bytes 2-12)
		std::string visitor_id(bytes.begin() + 2, bytes.begin() + 13);

		// Validate - should be 11 alphanumeric characters
		static const boost::regex re("^[A-Za-z0-9_-]{11}$");
		if (boost::regex_match(visitor_id, re)) { return visitor_id; }
	} catch (...) {
		// Ignore decoding errors
	}

	return std::nullopt;
}

// PoTokenDirector implementation
class PoTokenDirector::Impl {
   public:
	std::vector<std::shared_ptr<PoTokenProvider>> providers_;
	// Simple in-memory cache: key -> (token, expiry)
	std::map<std::string,
			 std::pair<std::string, std::chrono::steady_clock::time_point>>
		cache_;

	std::string make_cache_key(const PoTokenCacheSpec &spec) {
		std::ostringstream oss;
		for (const auto &[key, value] : spec.key_bindings) {
			oss << key << "=" << value << "&";
		}
		return oss.str();
	}

	std::optional<std::string> get_cached_token(const std::string &key) {
		auto it = cache_.find(key);
		if (it == cache_.end()) { return std::nullopt; }

		// Check expiry
		if (std::chrono::steady_clock::now() > it->second.second) {
			cache_.erase(it);
			return std::nullopt;
		}

		return it->second.first;
	}

	void cache_token(const std::string &key, const std::string &token,
					 int ttl_seconds) {
		auto expiry = std::chrono::steady_clock::now() +
					  std::chrono::seconds(ttl_seconds);
		cache_[key] = {token, expiry};
	}
};

PoTokenDirector::PoTokenDirector() : impl_(std::make_unique<Impl>()) {}

PoTokenDirector::~PoTokenDirector() = default;

void PoTokenDirector::register_provider(
	std::shared_ptr<PoTokenProvider> provider) {
	impl_->providers_.push_back(provider);
}

std::optional<std::string> PoTokenDirector::get_po_token(
	const PoTokenRequest &request) {
	// Try to get from cache first
	WebPoCacheSpecProvider webpo_provider;
	auto cache_spec = webpo_provider.generate_cache_spec(request);
	if (cache_spec && !request.bypass_cache) {
		auto key = impl_->make_cache_key(*cache_spec);
		auto cached = impl_->get_cached_token(key);
		if (cached) {
			spdlog::debug("PO Token retrieved from cache");
			return cached;
		}
	}

	// Try each provider
	for (auto &provider : impl_->providers_) {
		if (!provider->is_available()) { continue; }

		if (!provider->supports_request(request)) { continue; }

		spdlog::debug(
			"Requesting PO Token from provider: {}", provider->get_name());

		auto response = provider->request_pot(request);
		if (response && !response->po_token.empty()) {
			// Cache the token
			if (cache_spec) {
				auto key = impl_->make_cache_key(*cache_spec);
				int ttl = cache_spec->default_ttl;
				if (response->expires_at) {
					auto now =
						std::chrono::system_clock::now().time_since_epoch();
					auto expiry = std::chrono::seconds(*response->expires_at);
					auto remaining =
						std::chrono::duration_cast<std::chrono::seconds>(
							expiry -
							std::chrono::duration_cast<std::chrono::seconds>(
								now));
					ttl = static_cast<int>(remaining.count());
				}
				impl_->cache_token(key, response->po_token, ttl);
			}

			return response->po_token;
		}
	}

	return std::nullopt;
}

void PoTokenDirector::clear_cache() { impl_->cache_.clear(); }

// Utility functions namespace
namespace pot {

std::pair<std::string, ContentBindingType> get_webpo_content_binding(
	const PoTokenRequest &request, bool bind_to_visitor_id) {
	WebPoCacheSpecProvider provider;
	return provider.get_content_binding(request, bind_to_visitor_id);
}

std::optional<std::string> extract_visitor_id(const std::string &visitor_data) {
	WebPoCacheSpecProvider provider;
	return provider.extract_visitor_id(visitor_data);
}

std::optional<std::string> clean_pot(const std::string &po_token) {
	try {
		// URL decode using Boost.URL decode_view
		boost::urls::decode_view decoded_view(po_token);
		std::string decoded(decoded_view.begin(), decoded_view.end());

		// Base64 decode and re-encode to normalize
		auto bytes = base64_decode(decoded);
		return base64_encode(bytes);
	} catch (...) { return std::nullopt; }
}

bool is_webpo_client(const std::string &client_name) {
	for (const auto &client : WEBPO_CLIENTS) {
		if (client == client_name) { return true; }
	}
	return false;
}

}  // namespace pot

}  // namespace ytdlpp::youtube
