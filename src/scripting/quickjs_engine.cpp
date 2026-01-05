#include "quickjs_engine.hpp"

#include <spdlog/spdlog.h>

namespace ytdlpp::scripting {

JsEngine::JsEngine() {
	rt_ = JS_NewRuntime();
	if (!rt_) { throw std::runtime_error("Failed to create QuickJS runtime"); }
	ctx_ = JS_NewContext(rt_);
	if (!ctx_) {
		JS_FreeRuntime(rt_);
		throw std::runtime_error("Failed to create QuickJS context");
	}
}

JsEngine::~JsEngine() {
	if (ctx_) JS_FreeContext(ctx_);
	if (rt_) JS_FreeRuntime(rt_);
}

std::string JsEngine::get_exception_str() {
	JSValue exception_val = JS_GetException(ctx_);
	const char *str = JS_ToCString(ctx_, exception_val);
	std::string res = str ? str : "Unknown JS Exception";
	if (str) JS_FreeCString(ctx_, str);

	// QuickJS keeps the exception value, we should free it usually if we
	// handled it? Actually JS_GetException returns a value that we own? Use
	// docs. Yes, JS_GetException returns a new reference (or the value itself).
	// We must free exception_val.
	JS_FreeValue(ctx_, exception_val);
	return res;
}

void JsEngine::evaluate(const std::string &code) {
	// JS_Eval(ctx, input, input_len, filename, flags)
	JSValue ret = JS_Eval(
		ctx_, code.c_str(), code.length(), "<input>", JS_EVAL_TYPE_GLOBAL);

	if (JS_IsException(ret)) {
		std::string err = get_exception_str();
		JS_FreeValue(ctx_, ret);
		throw std::runtime_error("JS Evaluation failed: " + err);
	}

	JS_FreeValue(ctx_, ret);
}

std::string JsEngine::call_function(const std::string &func_name,
									const std::vector<std::string> &args) {
	// Get the function object
	JSValue global_obj = JS_GetGlobalObject(ctx_);
	JSValue func_obj = JS_GetPropertyStr(ctx_, global_obj, func_name.c_str());
	JS_FreeValue(ctx_, global_obj);	 // Can free global immediately? Yes,
									 // func_obj has its own ref count if found.

	if (!JS_IsFunction(ctx_, func_obj)) {
		JS_FreeValue(ctx_, func_obj);
		throw std::runtime_error(
			"Function not found or not a function: " + func_name);
	}

	// Convert args
	std::vector<JSValue> js_args;
	js_args.reserve(args.size());
	for (const auto &arg : args) {
		js_args.push_back(JS_NewString(ctx_, arg.c_str()));
	}

	// Call
	JSValue ret = JS_Call(
		ctx_, func_obj, JS_UNDEFINED, (int)js_args.size(), js_args.data());

	// Clean up args
	for (auto val : js_args) { JS_FreeValue(ctx_, val); }
	JS_FreeValue(ctx_, func_obj);

	if (JS_IsException(ret)) {
		std::string err = get_exception_str();
		JS_FreeValue(ctx_, ret);
		throw std::runtime_error("JS Call failed: " + err);
	}

	const char *res_str = JS_ToCString(ctx_, ret);
	std::string result = res_str ? res_str : "";
	if (res_str) JS_FreeCString(ctx_, res_str);
	JS_FreeValue(ctx_, ret);

	return result;
}

}  // namespace ytdlpp::scripting
