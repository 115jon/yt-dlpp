#pragma once

#include <quickjs.h>

#include <mutex>
#include <string>
#include <vector>
#include <ytdlpp/result.hpp>


namespace ytdlpp::scripting {

class JsEngine {
   public:
	JsEngine();
	~JsEngine();

	// Disable copy/move to avoid double-freeing context/runtime in this
	// simple wrapper
	JsEngine(const JsEngine &) = delete;
	JsEngine &operator=(const JsEngine &) = delete;
	JsEngine(JsEngine &&) = delete;
	JsEngine &operator=(JsEngine &&) = delete;

	// Evaluates arbitrary JS code.
	Result<void> evaluate(const std::string &code);

	// Calls a global function with string arguments. Returns string result.
	Result<std::string> call_function(const std::string &func_name,
									  const std::vector<std::string> &args);

   private:
	JSRuntime *rt_;
	JSContext *ctx_;
	std::mutex mutex_;

	std::string get_exception_str();
};

}  // namespace ytdlpp::scripting
