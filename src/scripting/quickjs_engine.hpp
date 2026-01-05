#pragma once

#include <quickjs.h>

#include <string>
#include <vector>

namespace ytdlpp::scripting {

class JsEngine {
   public:
	JsEngine();
	~JsEngine();

	// Disable copy/move to avoid double-freeing context/runtime in this simple
	// wrapper
	JsEngine(const JsEngine &) = delete;
	JsEngine &operator=(const JsEngine &) = delete;

	// Evaluates arbitrary JS code. Throws std::runtime_error on failure.
	void evaluate(const std::string &code);

	// Calls a global function with string arguments. Returns string result.
	std::string call_function(const std::string &func_name,
							  const std::vector<std::string> &args);

   private:
	JSRuntime *rt_;
	JSContext *ctx_;

	std::string get_exception_str();
};

}  // namespace ytdlpp::scripting
