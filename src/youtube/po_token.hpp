#pragma once

#include <boost/regex.hpp>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace ytdlpp::youtube {

// PO Token context types
enum class PoTokenContext {
	GVS,	 // Google Video Server
	PLAYER,	 // Player API
	SUBS,	 // Subtitles
};

// Content binding types for WebPO
enum class ContentBindingType {
	VISITOR_DATA,
	DATASYNC_ID,
	VIDEO_ID,
	VISITOR_ID,
};

// PO Token request structure
struct PoTokenRequest {
	PoTokenContext context;
	nlohmann::json innertube_context;
	std::string innertube_host = "www.youtube.com";
	std::string session_index;
	std::string player_url;
	bool is_authenticated = false;

	// Content binding parameters
	std::string visitor_data;
	std::string data_sync_id;
	std::string video_id;
	bool gvs_bind_to_video_id = false;

	// Request parameters
	std::map<std::string, std::string> request_headers;
	std::string request_proxy;
	bool bypass_cache = false;
};

// PO Token response structure
struct PoTokenResponse {
	std::string po_token;
	std::optional<int64_t> expires_at;
};

// Cache specification for PO Tokens
struct PoTokenCacheSpec {
	std::map<std::string, std::string> key_bindings;
	int default_ttl = 21600;  // 6 hours default
};

// Abstract base class for PO Token providers
class PoTokenProvider {
   public:
	virtual ~PoTokenProvider() = default;

	// Check if this provider is available
	virtual bool is_available() const = 0;

	// Get the provider name
	virtual std::string get_name() const = 0;

	// Request a PO Token
	virtual std::optional<PoTokenResponse> request_pot(
		const PoTokenRequest &request) = 0;

	// Check if this provider supports the given request
	virtual bool supports_request(const PoTokenRequest &request) const;

   protected:
	// Supported contexts (empty = all)
	std::vector<PoTokenContext> supported_contexts_;

	// Supported client names (empty = all)
	std::vector<std::string> supported_clients_;
};

// WebPO cache spec provider - generates cache keys for WebPO tokens
class WebPoCacheSpecProvider : public PoTokenProvider {
   public:
	WebPoCacheSpecProvider();

	bool is_available() const override { return true; }
	std::string get_name() const override { return "webpo"; }

	std::optional<PoTokenResponse> request_pot(
		const PoTokenRequest &request) override;

	// Generate cache spec for a request
	std::optional<PoTokenCacheSpec> generate_cache_spec(
		const PoTokenRequest &request) const;

	// Extract content binding from request
	std::pair<std::string, ContentBindingType> get_content_binding(
		const PoTokenRequest &request, bool bind_to_visitor_id = true) const;

	// Extract visitor ID from visitor_data
	std::optional<std::string> extract_visitor_id(
		const std::string &visitor_data) const;
};

// Director class that manages PO Token providers and caching
class PoTokenDirector {
   public:
	PoTokenDirector();
	~PoTokenDirector();

	// Register a provider
	void register_provider(std::shared_ptr<PoTokenProvider> provider);

	// Get a PO Token for a request
	std::optional<std::string> get_po_token(const PoTokenRequest &request);

	// Clear the cache
	void clear_cache();

   private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

// Utility functions
namespace pot {

// Get WebPO content binding for a request
std::pair<std::string, ContentBindingType> get_webpo_content_binding(
	const PoTokenRequest &request, bool bind_to_visitor_id = true);

// Extract visitor ID from base64-encoded visitor data
std::optional<std::string> extract_visitor_id(const std::string &visitor_data);

// Clean and validate a PO Token
std::optional<std::string> clean_pot(const std::string &po_token);

// Check if a client is a WebPO client
bool is_webpo_client(const std::string &client_name);

}  // namespace pot

}  // namespace ytdlpp::youtube
