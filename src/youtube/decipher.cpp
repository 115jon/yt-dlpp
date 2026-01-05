#include "decipher.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <regex>

namespace ytdlpp::youtube {

SigDecipherer::SigDecipherer(scripting::JsEngine &js) : js_(js) {}

bool SigDecipherer::load_functions(const std::string &player_code) {
	if (player_code.empty()) {
		spdlog::error("Player code is empty");
		return false;
	}
	spdlog::info("Scanning player script ({} bytes)...", player_code.size());

	// Manual search for Signature Function
	// Pattern: funcName=function(a){a=a.split("")
	std::string sig_body_start = "a=a.split(\"";
	size_t sig_pos = player_code.find(sig_body_start);
	if (sig_pos == std::string::npos) {
		// Try single quotes
		sig_body_start = "a=a.split('";
		sig_pos = player_code.find(sig_body_start);
	}

	// Sometimes it's a.split("") without assignment if chained? No, yt-dlp says
	// a=a.split

	bool found_sig = false;
	if (sig_pos != std::string::npos) {
		// Search backwards for "function(a){"
		// Expected structure: Name=function(a){...a=a.split
		// We look for "function" before sig_pos
		size_t func_def = player_code.rfind("function", sig_pos);
		if (func_def != std::string::npos) {
			// Check if it looks like "Name=function"
			// We search for "=" before "function"
			size_t eq_pos = player_code.rfind('=', func_def);
			if (eq_pos != std::string::npos) {
				// The name is between start of line or var and eq_pos
				// But in minified code: "var Name=function" or ";Name=function"
				// or "{Name=function" scan backwards from eq_pos to find
				// non-identifier char
				size_t name_end = eq_pos;
				size_t name_start = name_end;
				while (name_start > 0) {
					char c = player_code[name_start - 1];
					if (isalnum(c) || c == '$') {
						name_start--;
					} else {
						break;
					}
				}
				if (name_end > name_start) {
					sig_func_name_ =
						player_code.substr(name_start, name_end - name_start);
					found_sig = true;
					spdlog::info(
						"Found signature function name: {}", sig_func_name_);
				}
			}
		}
	}

	if (!found_sig) {
		spdlog::error("Could not find signature function via string search.");
		return false;
	}

	// Manual search for N Parameter Function using Regex
	// We iterate through several known patterns used by YouTube.
	std::vector<std::regex> n_regexes = {
		// Standard pattern: Func=function(a){...a.split("")...}
		std::regex(
			R"(([a-zA-Z0-9$]+)=function\(([a-zA-Z0-9$]+)\)\{[^}]*?\2\.split\((?:""|''))"),
		// Assignment pattern: Func=function(a){...var b=a.split("")...}
		std::regex(
			R"(([a-zA-Z0-9$]+)=function\(([a-zA-Z0-9$]+)\)\{[^}]*?var\s+[a-zA-Z0-9$]+\s*=\s*\2\.split\((?:""|''))"),
		// Direct split pattern without assignment (rare but possible in
		// minified code)
		std::regex(
			R"(([a-zA-Z0-9$]+)=function\(([a-zA-Z0-9$]+)\)\{[^}]*?\2\.split\((?:""|''))"),
		// Pattern where function name matches common obfuscated names
		// (fallback) - risky but helpful
		// std::regex(R"((b|l|p)=function\((a)\)\{a=a\.split\(\"\"\))")
	};

	bool found_n = false;
	std::smatch n_match;

	for (const auto &re : n_regexes) {
		if (std::regex_search(player_code, n_match, re)) {
			n_func_name_ = n_match[1].str();
			found_n = true;
			spdlog::info("Found n-parameter function name: {} (via regex)",
						 n_func_name_);
			break;
		}
	}

	if (!found_n) {
		// Fallback: Look for specific structure usually associated with N-algo
		// "b=a.split("")"
		std::string split_pattern = "a.split(\"";
		size_t split_pos = player_code.find(split_pattern);
		// This is weak, but we can try to find the enclosing function
		// For now, let's just warn.
		spdlog::warn("Could not find n-function via regex.");
	}

	// Extract bodies
	try {
		spdlog::info("Extracting signature function body...");
		std::string sig_code = extract_function(player_code, sig_func_name_);

		// Helper search
		// Regex is usually fine on small strings, but extracting helper name
		// from sig_code (small) is safe.
		std::regex helper_re(R"(([a-zA-Z0-9$]+)\.[a-zA-Z0-9$]+\(a,)");
		std::string helper_code;
		std::smatch helper_match;
		if (std::regex_search(sig_code, helper_match, helper_re)) {
			std::string helper_name = helper_match[1].str();
			spdlog::info("Found signature helper object: {}", helper_name);
			helper_code = extract_helper_object(player_code, helper_name);
		}

		std::string n_code;
		if (found_n) {
			spdlog::info("Extracting n-function body...");
			n_code = extract_function(player_code, n_func_name_);
		}

		std::string full_script = helper_code + "\n" + sig_code + "\n" + n_code;
		spdlog::info("Loading extracted script into JS engine ({} bytes)",
					 full_script.length());
		js_.evaluate(full_script);

		if (found_n && !n_func_name_.empty()) {
			std::string wrapper =
				"function " + n_func_name_ +
				"_wrapper(a) { var r = " + n_func_name_ +
				"(a); return Array.isArray(r) ? r.join('') : r; };";
			js_.evaluate(wrapper);
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
	try {
		return js_.call_function(sig_func_name_, {signature});
	} catch (const std::exception &e) {
		spdlog::error("Signature decipher failed: {}", e.what());
		return signature;
	}
}

std::string SigDecipherer::transform_n(const std::string &n) {
	if (n_func_name_.empty()) return n;
	try {
		return js_.call_function(n_func_name_, {n});
	} catch (const std::exception &e) {
		spdlog::warn("N-parameter transformation failed: {}", e.what());
		return n;
	}
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
