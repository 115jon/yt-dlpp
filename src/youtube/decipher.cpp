#include "decipher.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <regex>

namespace ytdlpp::youtube {

// =============================================================================
// OPTIMIZED STATIC REGEX PATTERNS
// =============================================================================
// These are compiled once at program startup instead of on every call.
// Using std::regex::optimize for better matching performance at the cost of
// slower initial compilation (which happens only once).
// =============================================================================

namespace {

// N-parameter function patterns - ordered by likelihood of matching
struct NParamRegexes {
	std::regex standard;
	std::regex assignment;
	std::regex direct;

	NParamRegexes()
		// Standard pattern: Func=function(a){...a.split("")...}
		: standard(
			  R"RE(([a-zA-Z0-9$]+)=function\(([a-zA-Z0-9$]+)\)\{[^}]*?\2\.split\((?:""|'')))RE",
			  std::regex::optimize),
		  // Assignment pattern: Func=function(a){...var b=a.split("")...}
		  assignment(
			  R"RE(([a-zA-Z0-9$]+)=function\(([a-zA-Z0-9$]+)\)\{[^}]*?var\s+[a-zA-Z0-9$]+\s*=\s*\2\.split\((?:""|'')))RE",
			  std::regex::optimize),
		  // Direct split pattern (duplicate of standard, kept for
		  // compatibility)
		  direct(
			  R"RE(([a-zA-Z0-9$]+)=function\(([a-zA-Z0-9$]+)\)\{[^}]*?\2\.split\((?:""|'')))RE",
			  std::regex::optimize) {}
};

// Helper object pattern - extracts object name from signature function body
// This regex runs on small strings (extracted function body ~500-2000 bytes)
// so performance here is less critical
struct HelperRegex {
	std::regex pattern;

	HelperRegex()
		: pattern(R"RE(([a-zA-Z0-9$]+)\.[a-zA-Z0-9$]+\(a,)RE",
				  std::regex::optimize) {}
};

// Get static regex instances (initialized once on first use)
const NParamRegexes &get_n_param_regexes() {
	static const NParamRegexes instance;
	return instance;
}

const HelperRegex &get_helper_regex() {
	static const HelperRegex instance;
	return instance;
}

}  // namespace

SigDecipherer::SigDecipherer(scripting::JsEngine &js) : js_(js) {}

bool SigDecipherer::load_functions(const std::string &player_code) {
	if (player_code.empty()) {
		spdlog::error("Player code is empty");
		return false;
	}
	spdlog::info("Scanning player script ({} bytes)...", player_code.size());

	// =========================================================================
	// SIGNATURE FUNCTION DETECTION (String-based - fast)
	// =========================================================================
	// Pattern: funcName=function(a){a=a.split("")
	// We use string search which is much faster than regex for this pattern.

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
					if (isalnum(static_cast<unsigned char>(c)) || c == '$') {
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

	// =========================================================================
	// N-PARAMETER FUNCTION DETECTION (Regex-based - uses static patterns)
	// =========================================================================
	// We use pre-compiled static regex patterns for better performance.

	const auto &n_regexes = get_n_param_regexes();
	bool found_n = false;
	std::smatch n_match;

	// Try each pattern in order of likelihood
	if (std::regex_search(player_code, n_match, n_regexes.standard)) {
		n_func_name_ = n_match[1].str();
		found_n = true;
		spdlog::info("Found n-parameter function name: {} (standard pattern)",
					 n_func_name_);
	} else if (std::regex_search(player_code, n_match, n_regexes.assignment)) {
		n_func_name_ = n_match[1].str();
		found_n = true;
		spdlog::info("Found n-parameter function name: {} (assignment pattern)",
					 n_func_name_);
	} else if (std::regex_search(player_code, n_match, n_regexes.direct)) {
		n_func_name_ = n_match[1].str();
		found_n = true;
		spdlog::info("Found n-parameter function name: {} (direct pattern)",
					 n_func_name_);
	}

	if (!found_n) { spdlog::warn("Could not find n-function via regex."); }

	// =========================================================================
	// FUNCTION EXTRACTION
	// =========================================================================
	try {
		spdlog::info("Extracting signature function body...");
		std::string sig_code = extract_function(player_code, sig_func_name_);

		// Helper object search - regex on small string is acceptable
		const auto &helper_re = get_helper_regex();
		std::string helper_code;
		std::smatch helper_match;
		if (std::regex_search(sig_code, helper_match, helper_re.pattern)) {
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
		auto eval_res = js_.evaluate(full_script);
		if (eval_res.has_error()) {
			spdlog::error("Evaluating script failed");
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
	spdlog::error("Signature decipher failed: {}", res.error().message());
	return signature;
}

std::string SigDecipherer::transform_n(const std::string &n) {
	if (n_func_name_.empty()) return n;
	auto res = js_.call_function(n_func_name_, {n});
	if (res.has_value()) return res.value();
	spdlog::warn(
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
