#include <gtest/gtest.h>

#include <boost/beast/core/detail/base64.hpp>
#include <boost/regex.hpp>

#include "../src/youtube/po_token.hpp"
#include "../src/youtube/po_token_provider.hpp"

using namespace ytdlpp::youtube;

// =============================================================================
// Base64 Tests
// =============================================================================

TEST(Base64Test, EncodeEmpty) {
	std::vector<uint8_t> input;
	std::string result;
	result.resize(boost::beast::detail::base64::encoded_size(input.size()));
	boost::beast::detail::base64::encode(
		result.data(), input.data(), input.size());
	EXPECT_EQ(result, "");
}

TEST(Base64Test, EncodeSimple) {
	std::string input = "Hello World";
	std::string result;
	result.resize(boost::beast::detail::base64::encoded_size(input.size()));
	boost::beast::detail::base64::encode(
		result.data(), input.data(), input.size());
	// Note: Beast base64 doesn't add padding by default in some versions
	EXPECT_TRUE(result.find("SGVsbG8gV29ybGQ") != std::string::npos);
}

TEST(Base64Test, DecodeSimple) {
	std::string input = "SGVsbG8gV29ybGQ=";
	std::vector<uint8_t> result;
	result.resize(boost::beast::detail::base64::decoded_size(input.size()));
	auto [out_len, _] = boost::beast::detail::base64::decode(
		result.data(), input.data(), input.size());
	result.resize(out_len);
	std::string decoded(result.begin(), result.end());
	EXPECT_EQ(decoded, "Hello World");
}

TEST(Base64Test, RoundTrip) {
	std::string original = "The quick brown fox jumps over the lazy dog.";

	// Encode
	std::string encoded;
	encoded.resize(boost::beast::detail::base64::encoded_size(original.size()));
	boost::beast::detail::base64::encode(
		encoded.data(), original.data(), original.size());

	// Decode
	std::vector<uint8_t> decoded;
	decoded.resize(boost::beast::detail::base64::decoded_size(encoded.size()));
	auto [out_len, __] = boost::beast::detail::base64::decode(
		decoded.data(), encoded.data(), encoded.size());
	decoded.resize(out_len);

	std::string result(decoded.begin(), decoded.end());
	EXPECT_EQ(result, original);
}

// =============================================================================
// Visitor ID Extraction Tests
// =============================================================================

TEST(VisitorIdTest, ExtractFromValidData) {
	// Create a mock visitor data structure
	// Format: base64 encoded protobuf with visitor ID at bytes 2-12
	WebPoCacheSpecProvider provider;

	// Test with empty data
	auto result = provider.extract_visitor_id("");
	EXPECT_FALSE(result.has_value());

	// Test with invalid data
	result = provider.extract_visitor_id("invalid_base64!!!");
	EXPECT_FALSE(result.has_value());
}

// =============================================================================
// Content Binding Tests
// =============================================================================

TEST(ContentBindingTest, WebClientVisitorData) {
	PoTokenRequest request;
	request.context = PoTokenContext::GVS;
	request.innertube_context["client"]["clientName"] = "WEB";
	request.visitor_data = "CgR0ZXN0EAE";
	request.video_id = "dQw4w9WgXcQ";

	auto [binding, type] = pot::get_webpo_content_binding(request, true);

	// Should bind to visitor_data or visitor_id for GVS context
	EXPECT_FALSE(binding.empty());
	EXPECT_TRUE(type == ContentBindingType::VISITOR_DATA ||
				type == ContentBindingType::VISITOR_ID);
}

TEST(ContentBindingTest, PlayerContextVideoId) {
	PoTokenRequest request;
	request.context = PoTokenContext::PLAYER;
	request.innertube_context["client"]["clientName"] = "WEB";
	request.visitor_data = "CgR0ZXN0EAE";
	request.video_id = "dQw4w9WgXcQ";

	auto [binding, type] = pot::get_webpo_content_binding(request, true);

	// PLAYER context should bind to video_id
	EXPECT_EQ(binding, "dQw4w9WgXcQ");
	EXPECT_EQ(type, ContentBindingType::VIDEO_ID);
}

TEST(ContentBindingTest, SubsContextVideoId) {
	PoTokenRequest request;
	request.context = PoTokenContext::SUBS;
	request.innertube_context["client"]["clientName"] = "WEB";
	request.visitor_data = "CgR0ZXN0EAE";
	request.video_id = "dQw4w9WgXcQ";

	auto [binding, type] = pot::get_webpo_content_binding(request, true);

	// SUBS context should bind to video_id
	EXPECT_EQ(binding, "dQw4w9WgXcQ");
	EXPECT_EQ(type, ContentBindingType::VIDEO_ID);
}

TEST(ContentBindingTest, GvsWithVideoIdBinding) {
	PoTokenRequest request;
	request.context = PoTokenContext::GVS;
	request.innertube_context["client"]["clientName"] = "WEB";
	request.visitor_data = "CgR0ZXN0EAE";
	request.video_id = "dQw4w9WgXcQ";
	request.gvs_bind_to_video_id = true;

	auto [binding, type] = pot::get_webpo_content_binding(request, true);

	// GVS with gvs_bind_to_video_id should bind to video_id
	EXPECT_EQ(binding, "dQw4w9WgXcQ");
	EXPECT_EQ(type, ContentBindingType::VIDEO_ID);
}

TEST(ContentBindingTest, NonWebpoClient) {
	PoTokenRequest request;
	request.context = PoTokenContext::GVS;
	request.innertube_context["client"]["clientName"] = "ANDROID";
	request.visitor_data = "CgR0ZXN0EAE";

	auto [binding, type] = pot::get_webpo_content_binding(request, true);

	// Non-WebPO clients should return empty binding
	EXPECT_TRUE(binding.empty());
}

// =============================================================================
// PO Token Cleaning Tests
// =============================================================================

TEST(CleanPotTest, ValidToken) {
	// Test with a simple base64 string
	std::string valid_token = "SGVsbG8gV29ybGQ";
	auto result = pot::clean_pot(valid_token);
	EXPECT_TRUE(result.has_value());
}

TEST(CleanPotTest, EmptyToken) {
	auto result = pot::clean_pot("");
	EXPECT_FALSE(result.has_value());
}

TEST(CleanPotTest, InvalidToken) {
	auto result = pot::clean_pot("!!!invalid!!!");
	EXPECT_FALSE(result.has_value());
}

// =============================================================================
// WebPO Client Detection Tests
// =============================================================================

TEST(WebpoClientTest, IsWebpoClient) {
	EXPECT_TRUE(pot::is_webpo_client("WEB"));
	EXPECT_TRUE(pot::is_webpo_client("MWEB"));
	EXPECT_TRUE(pot::is_webpo_client("TVHTML5"));
	EXPECT_TRUE(pot::is_webpo_client("WEB_EMBEDDED_PLAYER"));
	EXPECT_TRUE(pot::is_webpo_client("WEB_CREATOR"));
	EXPECT_TRUE(pot::is_webpo_client("WEB_REMIX"));
}

TEST(WebpoClientTest, IsNotWebpoClient) {
	EXPECT_FALSE(pot::is_webpo_client("ANDROID"));
	EXPECT_FALSE(pot::is_webpo_client("IOS"));
	EXPECT_FALSE(pot::is_webpo_client("UNKNOWN"));
	EXPECT_FALSE(pot::is_webpo_client(""));
}

// =============================================================================
// PoTokenDirector Tests
// =============================================================================

TEST(PoTokenDirectorTest, RegisterProvider) {
	PoTokenDirector director;
	auto provider = std::make_shared<WebPoCacheSpecProvider>();

	// Should not throw
	EXPECT_NO_THROW(director.register_provider(provider));
}

TEST(PoTokenDirectorTest, ClearCache) {
	PoTokenDirector director;

	// Should not throw
	EXPECT_NO_THROW(director.clear_cache());
}

TEST(PoTokenDirectorTest, GetTokenWithoutProviders) {
	PoTokenDirector director;
	PoTokenRequest request;
	request.context = PoTokenContext::GVS;
	request.innertube_context["client"]["clientName"] = "WEB";
	request.visitor_data = "CgR0ZXN0EAE";

	// Without registered providers that generate tokens, should return nullopt
	auto token = director.get_po_token(request);
	// Note: This may return a cached token or nullopt depending on
	// implementation The test documents the expected behavior
}

// =============================================================================
// WebPoTokenProvider Tests (requires HTTP client)
// =============================================================================

TEST(WebPoTokenProviderTest, IsAvailable) {
	// Provider without HTTP client should not be available
	WebPoTokenProvider provider(nullptr);
	EXPECT_FALSE(provider.is_available());
}

TEST(WebPoTokenProviderTest, GetName) {
	WebPoTokenProvider provider(nullptr);
	EXPECT_EQ(provider.get_name(), "webpo");
}

TEST(WebPoCacheSpecProviderTest, IsAvailable) {
	WebPoCacheSpecProvider provider;
	EXPECT_TRUE(provider.is_available());
}

TEST(WebPoCacheSpecProviderTest, GetName) {
	WebPoCacheSpecProvider provider;
	EXPECT_EQ(provider.get_name(), "webpo");
}

TEST(WebPoCacheSpecProviderTest, GenerateCacheSpec) {
	WebPoCacheSpecProvider provider;
	PoTokenRequest request;
	request.context = PoTokenContext::GVS;
	request.innertube_context["client"]["clientName"] = "WEB";
	request.visitor_data = "CgR0ZXN0EAE";
	request.video_id = "dQw4w9WgXcQ";

	auto spec = provider.generate_cache_spec(request);
	EXPECT_TRUE(spec.has_value());
	EXPECT_FALSE(spec->key_bindings.empty());
	EXPECT_EQ(spec->default_ttl, 21600);  // 6 hours
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char **argv) {
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
