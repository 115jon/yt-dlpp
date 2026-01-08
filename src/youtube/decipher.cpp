#include "decipher.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <boost/regex.hpp>

namespace ytdlpp::youtube {

namespace {

// N-parameter function patterns - ordered by likelihood of matching
// We use lazy initialization to avoid static initialization order issues

const boost::regex &get_n_param_pattern_1() {
	// Standard pattern: Func=function(a){...a.split("")...}
	// Uses backreference \2 to match the parameter name
	static const boost::regex pattern(
		R"(([a-zA-Z0-9$_]+)\s*=\s*function\s*\(\s*([a-zA-Z0-9$_]+)\s*\)\s*\{[^}]*?\2\.split\s*\(\s*["'][^"']*["']\s*\))",
		boost::regex::optimize);
	return pattern;
}

const boost::regex &get_n_param_pattern_2() {
	// Assignment pattern: Func=function(a){...var b=a.split("")...}
	static const boost::regex pattern(
		R"(([a-zA-Z0-9$_]+)\s*=\s*function\s*\(\s*([a-zA-Z0-9$_]+)\s*\)\s*\{[^}]*?var\s+[a-zA-Z0-9$_]+\s*=\s*\2\.split\s*\()",
		boost::regex::optimize);
	return pattern;
}

const boost::regex &get_n_param_pattern_3() {
	// Enhanced pattern - allows for more whitespace variations
	static const boost::regex pattern(
		R"(([a-zA-Z0-9$_]{1,30})\s*=\s*function\s*\(\s*([a-zA-Z0-9$_]{1,10})\s*\)\s*\{[^{}]*\2\.split\s*\()",
		boost::regex::optimize);
	return pattern;
}

// Helper object pattern - extracts object name from signature function body
const boost::regex &get_helper_pattern() {
	static const boost::regex pattern(
		R"(([a-zA-Z0-9$_]+)\.[a-zA-Z0-9$_]+\s*\(\s*a\s*,)",
		boost::regex::optimize);
	return pattern;
}

}  // namespace

SigDecipherer::SigDecipherer(scripting::JsEngine &js) : js_(js) {}

bool SigDecipherer::load_functions(const std::string &player_code) {
	if (player_code.empty()) {
		spdlog::error("Player code is empty");
		return false;
	}
	spdlog::debug("Scanning player script ({} bytes)...", player_code.size());

	// =========================================================================
	// SIGNATURE FUNCTION DETECTION (String-based - fastest approach)
	// =========================================================================
	// Pattern: funcName=function(a){a=a.split("")
	// String search is faster than regex for this simple pattern.

	std::string sig_body_start = "a=a.split(\"";
	size_t sig_pos = player_code.find(sig_body_start);
	if (sig_pos == std::string::npos) {
		// Try single quotes
		sig_body_start = "a=a.split('";
		sig_pos = player_code.find(sig_body_start);
	}

	bool found_sig = false;
	if (sig_pos != std::string::npos) {
		// Search backwards for "function(a){"
		// Expected structure: Name=function(a){...a=a.split
		size_t func_def = player_code.rfind("function", sig_pos);
		if (func_def != std::string::npos) {
			// Check if it looks like "Name=function"
			size_t eq_pos = player_code.rfind('=', func_def);
			if (eq_pos != std::string::npos) {
				// Extract function name by scanning backwards from '='
				size_t name_end = eq_pos;
				size_t name_start = name_end;
				while (name_start > 0) {
					char c = player_code[name_start - 1];
					if (isalnum(static_cast<unsigned char>(c)) || c == '$' ||
						c == '_') {
						name_start--;
					} else {
						break;
					}
				}
				if (name_end > name_start) {
					sig_func_name_ =
						player_code.substr(name_start, name_end - name_start);
					found_sig = true;
					spdlog::debug(
						"Found signature function name: {}", sig_func_name_);
				}
			}
		}
	}

	if (!found_sig) {
		spdlog::debug("Could not find signature function via string search.");
		return false;
	}

	bool found_n = false;
	boost::smatch n_match;

	try {
		// Try each pattern in order of likelihood
		if (boost::regex_search(
				player_code, n_match, get_n_param_pattern_1())) {
			n_func_name_ = n_match[1].str();
			found_n = true;
			spdlog::debug("Found n-parameter function name: {} (pattern 1)",
						  n_func_name_);
		} else if (boost::regex_search(
					   player_code, n_match, get_n_param_pattern_2())) {
			n_func_name_ = n_match[1].str();
			found_n = true;
			spdlog::debug("Found n-parameter function name: {} (pattern 2)",
						  n_func_name_);
		} else if (boost::regex_search(
					   player_code, n_match, get_n_param_pattern_3())) {
			n_func_name_ = n_match[1].str();
			found_n = true;
			spdlog::debug("Found n-parameter function name: {} (pattern 3)",
						  n_func_name_);
		}
	} catch (const boost::regex_error &e) {
		spdlog::error("Regex error during n-function search: {}", e.what());
	}

	if (!found_n) {
		spdlog::debug(
			"Could not find n-function via regex. "
			"N-parameter throttling mitigation unavailable.");
	}

	try {
		spdlog::debug("Extracting signature function body...");
		std::string sig_code = extract_function(player_code, sig_func_name_);

		// Helper object search - regex on small string is very fast
		std::string helper_code;
		boost::smatch helper_match;
		if (boost::regex_search(sig_code, helper_match, get_helper_pattern())) {
			std::string helper_name = helper_match[1].str();
			spdlog::debug("Found signature helper object: {}", helper_name);
			helper_code = extract_helper_object(player_code, helper_name);
		}

		std::string n_code;
		if (found_n) {
			spdlog::debug("Extracting n-function body...");
			n_code = extract_function(player_code, n_func_name_);
		}

		std::string full_script = helper_code + "\n" + sig_code + "\n" + n_code;
		spdlog::debug("Loading extracted script into JS engine ({} bytes)",
					  full_script.length());
		auto eval_res = js_.evaluate(full_script);
		if (eval_res.has_error()) {
			spdlog::debug("Evaluating script failed");
			return false;
		}

		if (found_n && !n_func_name_.empty()) {
			std::string wrapper =
				"function " + n_func_name_ +
				"_wrapper(a) { var r = " + n_func_name_ +
				"(a); return Array.isArray(r) ? r.join('') : r; };";
			auto w_res = js_.evaluate(wrapper);
			if (w_res.has_error()) return false;
			n_func_name_ += "_wrapper";	 // Use the wrapper from now on
		}

		return true;

	} catch (const std::exception &e) {
		spdlog::error("Error loading functions: {}", e.what());
		return false;
	}
}

std::string SigDecipherer::decipher_signature(const std::string &signature) {
	if (sig_func_name_.empty()) return signature;
	auto res = js_.call_function(sig_func_name_, {signature});
	if (res.has_value()) return res.value();
	// Use debug level - ANDROID/iOS clients don't need signature deciphering
	spdlog::debug("Signature decipher failed: {}", res.error().message());
	return signature;
}

std::string SigDecipherer::transform_n(const std::string &n) {
	if (n_func_name_.empty()) return n;
	auto res = js_.call_function(n_func_name_, {n});
	if (res.has_value()) return res.value();
	// Use debug level - expected to fail for some clients
	spdlog::debug(
		"N-parameter transformation failed: {}", res.error().message());
	return n;
}

std::string SigDecipherer::extract_function(const std::string &code,
											const std::string &func_name) {
	std::string search = func_name + "=function";
	size_t start = code.find(search);
	if (start == std::string::npos)
		throw std::runtime_error("Function start not found: " + func_name);

	size_t open = code.find('{', start);
	if (open == std::string::npos)
		throw std::runtime_error("Function body start not found");

	int balance = 1;
	size_t pos = open + 1;
	while (balance > 0 && pos < code.length()) {
		if (code[pos] == '{')
			balance++;
		else if (code[pos] == '}')
			balance--;
		pos++;
	}

	if (balance != 0) throw std::runtime_error("Unbalanced braces");
	return code.substr(start, pos - start) + ";";
}

std::string SigDecipherer::extract_helper_object(
	const std::string &code, const std::string &object_name) {
	std::string search = "var " + object_name + "={";
	size_t start = code.find(search);
	if (start == std::string::npos) {
		search = object_name + "={";
		start = code.find(search);
		if (start == std::string::npos)
			throw std::runtime_error("Helper object not found: " + object_name);
	}

	size_t open = code.find('{', start);
	int balance = 1;
	size_t pos = open + 1;
	while (balance > 0 && pos < code.length()) {
		if (code[pos] == '{')
			balance++;
		else if (code[pos] == '}')
			balance--;
		pos++;
	}

	return code.substr(start, pos - start) + ";";
}

}  // namespace ytdlpp::youtube
