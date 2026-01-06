#pragma once

#include <quickjs.h>

#include <boost/asio.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <ytdlpp/result.hpp>

namespace ytdlpp::scripting {

// =============================================================================
// BYTECODE CACHE CONFIGURATION
// =============================================================================
// QuickJS bytecode provides ~10x faster script loading by skipping parsing.
// Bytecode is CPU-dependent (endianness, word length) and version-specific.
// =============================================================================

struct BytecodeConfig {
	// Size constants
	static constexpr size_t kMB = 1024 * 1024;
	static constexpr size_t kKB = 1024;

	// Maximum memory for JS runtime (default: 32MB, reasonable for most
	// platforms)
	size_t max_memory = 32 * kMB;

	// Maximum stack size (default: 1MB)
	size_t max_stack_size = 1 * kMB;

	// Enable bytecode caching
	bool enable_bytecode_cache = true;
};

class JsEngine {
   public:
	explicit JsEngine(boost::asio::any_io_executor ex,
					  BytecodeConfig config = {});
	~JsEngine();

	// Disable copy/move to avoid double-freeing context/runtime
	JsEngine(const JsEngine &) = delete;
	JsEngine &operator=(const JsEngine &) = delete;
	JsEngine(JsEngine &&) = delete;
	JsEngine &operator=(JsEngine &&) = delete;

	// Evaluates arbitrary JS code.
	Result<void> evaluate(const std::string &code);

	// Evaluates JS code with bytecode caching.
	// If bytecode is provided, it will be loaded directly (skipping parsing).
	// If bytecode generation is requested, returns compiled bytecode.
	Result<void> evaluate_with_cache(
		const std::string &code,
		const std::optional<std::vector<uint8_t>> &cached_bytecode =
			std::nullopt,
		std::optional<std::vector<uint8_t>> *out_bytecode = nullptr);

	// Calls a global function with string arguments. Returns string result.
	Result<std::string> call_function(const std::string &func_name,
									  const std::vector<std::string> &args);

	// Get runtime memory usage (useful for diagnostics)
	size_t get_memory_usage() const;

   private:
	boost::asio::any_io_executor ex_;
	JSRuntime *rt_ = nullptr;
	JSContext *ctx_ = nullptr;
	std::mutex mutex_;
	BytecodeConfig config_;

	std::string get_exception_str();

	// Internal helpers for bytecode handling
	Result<void> load_bytecode(const std::vector<uint8_t> &bytecode);
	Result<std::vector<uint8_t>> compile_to_bytecode(const std::string &code);
};

}  // namespace ytdlpp::scripting
