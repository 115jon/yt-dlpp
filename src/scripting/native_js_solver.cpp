#include "native_js_solver.hpp"

#include <spdlog/spdlog.h>

#include <boost/regex.hpp>
#include <cstring>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <vector>

#include "js_engine.hpp"

using json = nlohmann::json;

namespace ytdlpp {

NativeJsSolver::NativeJsSolver(scripting::JsEngine *js) : js_(js) {}

bool NativeJsSolver::init() {
	initialized_ = true;
	return true;
}

// --- Static Helpers for Dynamic Resolution ---
namespace {

std::string find_sig_function_impl(const std::string &code, const json &obf) {
	std::string arr = obf.value("name", "");
	int set_idx = obf.value("setIdx", -1);
	int sig_idx = obf.value("sigIdx", -1);
	int sig_cipher_idx = obf.value("sigCipherIdx", -1);

	// Build access patterns for 'set'
	std::vector<std::string> set_accessors;
	set_accessors.push_back("\\.set");
	set_accessors.push_back("\\[[\"']set[\"']\\]");
	if (!arr.empty() && set_idx >= 0) {
		set_accessors.push_back(
			"\\[" + arr + "\\[" + std::to_string(set_idx) + "\\]\\]");
	}

	// Build value patterns for 'signature' keys
	std::vector<std::string> key_patterns = {
		R"(["']signature["'])", R"(["']signatureCipher["'])"};
	if (!arr.empty()) {
		if (sig_idx >= 0)
			key_patterns.push_back(
				arr + "\\[" + std::to_string(sig_idx) + "\\]");
		if (sig_cipher_idx >= 0)
			key_patterns.push_back(
				arr + "\\[" + std::to_string(sig_cipher_idx) + "\\]");
	}

	std::string set_part = "(?:";
	for (size_t i = 0; i < set_accessors.size(); ++i) {
		set_part +=
			set_accessors[i] + (i < set_accessors.size() - 1 ? "|" : "");
	}
	set_part += ")";

	std::string key_part = "(?:";
	for (size_t i = 0; i < key_patterns.size(); ++i) {
		key_part += key_patterns[i] + (i < key_patterns.size() - 1 ? "|" : "");
	}
	key_part += ")";

	// Regex: set_part + `\(\s*` + key_part + `\s*,\s*([\w$]+)\)`
	std::string re_str =
		set_part + "\\(\\s*" + key_part + "\\s*,\\s*([\\w$]+)\\)";
	try {
		boost::regex re(re_str);
		boost::smatch m;
		if (boost::regex_search(code, m, re)) return m[1];
	} catch (const std::exception &e) {
		spdlog::error("[native-solver] Regex error (sig): {}", e.what());
	}

	std::string re_str_call =
		set_part + "\\(\\s*" + key_part + "\\s*,\\s*([\\w$]+)\\(";
	try {
		boost::regex re(re_str_call);
		boost::smatch m;
		if (boost::regex_search(code, m, re)) return m[1];
	} catch (...) {}

	static const boost::regex re3(
		R"((?:var\s+)?([\w$]+)\s*=\s*function\(\s*[\w$]+\s*\)\s*\{\s*[\w$]+\s*=\s*[\w$]+\.split\(\"\"\))");
	boost::smatch m;
	if (boost::regex_search(code, m, re3)) return m[1];

	return "";
}

std::string find_n_function_impl(const std::string &code, const json &obf) {
	std::string arr = obf.value("name", "");
	int get_idx = obf.value("getIdx", -1);
	int n_idx = obf.value("nIdx", -1);

	std::vector<std::string> get_accessors;
	get_accessors.push_back("\\.get");
	get_accessors.push_back("\\[[\"']get[\"']\\]");
	if (!arr.empty() && get_idx >= 0) {
		get_accessors.push_back(
			"\\[" + arr + "\\[" + std::to_string(get_idx) + "\\]\\]");
	}

	std::vector<std::string> key_patterns = {R"(["']n["'])"};
	if (!arr.empty() && n_idx >= 0) {
		key_patterns.push_back(arr + "\\[" + std::to_string(n_idx) + "\\]");
	}

	std::string get_part = "(?:";
	for (size_t i = 0; i < get_accessors.size(); ++i) {
		get_part +=
			get_accessors[i] + (i < get_accessors.size() - 1 ? "|" : "");
	}
	get_part += ")";

	std::string key_part = "(?:";
	for (size_t i = 0; i < key_patterns.size(); ++i) {
		key_part += key_patterns[i] + (i < key_patterns.size() - 1 ? "|" : "");
	}
	key_part += ")";

	std::string re_str =
		"\\b([\\w$]+)\\s*=\\s*[\\w$]+" + get_part + "\\(\\s*" + key_part +
		"\\)(?:[\\s\\S]*?)\\1\\s*=\\s*([\\w$]+)(?:\\[(\\d+)\\])?\\(\\1\\)";
	try {
		boost::regex re(re_str);
		boost::smatch m;
		if (boost::regex_search(code, m, re)) {
			if (m[3].matched) {
				return std::string(m[2]) + "[" + std::string(m[3]) + "]";
			}
			return m[2];
		}
	} catch (const std::exception &e) {
		spdlog::error("[native-solver] Regex error (n): {}", e.what());
	}

	static const boost::regex re2(
		R"((?:b|p|a)\s*=\s*([a-zA-Z0-9$]+)\((?:b|p|a)\))");
	boost::smatch m;
	if (boost::regex_search(code, m, re2)) return m[1];

	return "";
}

}  // namespace

bool NativeJsSolver::load_player(const std::string &player_code) {
	ready_ = false;

	if (player_code.empty()) {
		spdlog::error("[native-solver] Player code is empty");
		return false;
	}

	spdlog::debug("[native-solver] Processing player script ({} bytes)...",
				  player_code.size());

	std::string body = extract_iife_body(player_code);
	if (body.empty()) {
		spdlog::error("[native-solver] Failed to extract IIFE body");
		return false;
	}
	spdlog::debug(
		"[native-solver] IIFE body extracted ({} bytes)", body.size());

	auto statements = split_toplevel_statements(body);
	spdlog::debug("[native-solver] Split into {} top-level statements",
				  statements.size());

	std::string filtered_code = filter_statements(statements);
	spdlog::debug("[native-solver] Filtered code processing ({} bytes)",
				  filtered_code.size());

	{
		std::ofstream out("f:/dev/C++/yt-dlpp/debug_filtered.js");
		out << filtered_code;
	}

	// 4. Load browser stubs with Proxy
	const std::string stubs = R"(
        var _dummyFunc = function(){ return _dummyProxy; };
        var _dummyHandler = {
            get: function(t,p) {
                if (p === Symbol.toPrimitive || p === 'toString') return function(){return "";};
                if (p === 'length') return 0;
                return _dummyProxy;
            },
            set: function(){ return true; },
            apply: function(){ return _dummyProxy; },
            construct: function(){ return _dummyProxy; }
        };
        var _dummyProxy = new Proxy(_dummyFunc, _dummyHandler);

		var _realDoc = {
			createElement: function() {
                return { innerHTML: '', style: {}, appendChild: function(){}, setAttribute: function(){} };
            },
			write: function() {},
			cookie: '',
            getElementById: function(){ return _dummyProxy; },
            getElementsByTagName: function(){ return []; },
            body: _dummyProxy,
            head: _dummyProxy,
            documentElement: { style: {} }
		};
        var document = new Proxy(_realDoc, {
            get: function(t,p) { if(p in t) return t[p]; return _dummyProxy; },
            set: function(t,p,v) { t[p]=v; return true; }
        });

		var _realWindow = {
            location: { hostname: 'www.youtube.com', protocol: 'https:', href: 'https://www.youtube.com/' },
            document: document,
            navigator: { userAgent: 'Mozilla/5.0' },
            Intl: {
                NumberFormat: function() {
                    var f = function(n){ return ""+n; };
                    return { format: f };
                },
                DateTimeFormat: function() { return { format: function(d){ return d.toString(); } }; }
            },
            history: { pushState: function(){}, replaceState: function(){} },
            screen: { width: 1280, height: 720 },
            localStorage: { getItem: function(){ return null; }, setItem: function(){} },
            sessionStorage: { getItem: function(){ return null; }, setItem: function(){} },
            Error: Error,
            TypeError: TypeError,
            XMLHttpRequest: function(){
                 return {
                     open: function(){},
                     send: function(){},
                     setRequestHeader: function(){},
                     abort: function(){}
                 };
            }
        };

        // Static method required by some polyfills
        _realWindow.Intl.NumberFormat.supportedLocalesOf = function(){ return []; };

        var window = new Proxy(_realWindow, {
             get: function(t,p) { if(p in t) return t[p]; return _dummyProxy; },
             set: function(t,p,v) { t[p]=v; return true; }
        });

		var location = window.location;
		var navigator = window.navigator;
        var localStorage = window.localStorage;
        var sessionStorage = window.sessionStorage;
        var history = window.history;
        var screen = window.screen;
        var Intl = window.Intl;

		var g = window;
		var _yt_player = window;

        // Expose critical globals
        globalThis.window = window;
        globalThis.document = document;
        globalThis.location = window.location;
        globalThis.navigator = window.navigator;
        globalThis.XMLHttpRequest = _realWindow.XMLHttpRequest;
        globalThis.Intl = window.Intl;
	)";
	(void)js_->evaluate(stubs);

	// 5. Load safe code iteratively
	int success_count = 0;
	int fail_count = 0;
	for (const auto &stmt : statements) {
		bool keep = true;

		if (stmt.rfind("try", 0) == 0)
			keep = false;
		else if (stmt.rfind("if", 0) == 0)
			keep = false;
		else if (stmt.rfind("return", 0) == 0)
			keep = false;
		else if (stmt.rfind("throw", 0) == 0)
			keep = false;
		else if (stmt.rfind("while", 0) == 0)
			keep = false;
		else if (stmt.rfind("do", 0) == 0)
			keep = false;
		else if (stmt.rfind("switch", 0) == 0)
			keep = false;
		else if (stmt.rfind("break", 0) == 0)
			keep = false;
		else if (stmt.rfind("continue", 0) == 0)
			keep = false;

		// Always keep 'for' loops as they likely contain definitions
		if (stmt.rfind("for", 0) == 0) keep = true;

		if (keep) {
			auto res = js_->evaluate(stmt);
			if (res.has_error()) {
				fail_count++;
			} else {
				success_count++;
			}
		}
	}
	spdlog::info("[native-solver] Executed statements: {} success, {} failed",
				 success_count, fail_count);

	// 6. Inspect environment for obfuscation
	std::string inspect_code = R"(
		(function() {
			var res = { name: "", sigIdx: -1, sigCipherIdx: -1, nIdx: -1, setIdx: -1, getIdx: -1 };
			var arrName = "";
			for (var k in globalThis) {
				try {
					if (Array.isArray(globalThis[k]) && globalThis[k].length > 10) {
						if (globalThis[k].indexOf("signatureCipher") > -1 || globalThis[k].indexOf("signature") > -1) {
							arrName = k;
							break;
						}
					}
				} catch(e){}
			}
			if (arrName) {
				res.name = arrName;
				var arr = globalThis[arrName];
				res.sigIdx = arr.indexOf("signature");
				res.sigCipherIdx = arr.indexOf("signatureCipher");
				res.nIdx = arr.indexOf("n");
				res.setIdx = arr.indexOf("set");
				res.getIdx = arr.indexOf("get");
			}
			return JSON.stringify(res);
		})()
	)";

	json obf_data;
	auto inspect_res = js_->evaluate_and_get(inspect_code);
	if (inspect_res.has_value()) {
		try {
			obf_data = json::parse(inspect_res.value());
			spdlog::info("[native-solver] Obfuscation detected: array='{}'",
						 obf_data.value("name", "none"));
		} catch (...) {}
	}

	// 7. Find function names
	std::string sig_func_name = find_sig_function_impl(filtered_code, obf_data);
	std::string n_func_name = find_n_function_impl(filtered_code, obf_data);

	if (sig_func_name.empty() || n_func_name.empty()) {
		spdlog::warn(
			"[native-solver] Failed to find some functions: sig='{}', n='{}'",
			sig_func_name, n_func_name);
	}

	if (sig_func_name.empty() && n_func_name.empty()) {
		spdlog::error("[native-solver] Failed to find any solver functions");
		return false;
	}

	spdlog::info("[native-solver] Found functions: sig='{}', n='{}'",
				 sig_func_name, n_func_name);

	// 8. Store function names
	std::string store_names =
		"globalThis._native_sig_func_name = '" + sig_func_name +
		"';\n"
		"globalThis._native_n_func_name = '" +
		n_func_name + "';";
	(void)js_->evaluate(store_names);

	ready_ = true;
	return true;
}

std::string NativeJsSolver::solve_sig(const std::string &encrypted_sig) const {
	if (!ready_) return encrypted_sig;
	std::string code = R"(
		(function() {
			try {
				if (!globalThis._native_sig_func_name || !globalThis[globalThis._native_sig_func_name]) return null;
				return globalThis[globalThis._native_sig_func_name](')" +
					   js_escape(encrypted_sig) + R"(');
			} catch (e) { return null; }
		})()
	)";
	auto res = js_->evaluate_and_get(code);
	if (res.has_value() && res.value() != "null" && !res.value().empty())
		return res.value();
	return encrypted_sig;
}

std::string NativeJsSolver::solve_n(const std::string &n_param) const {
	if (!ready_) return n_param;
	std::string code = R"(
		(function() {
			try {
				if (!globalThis._native_n_func_name || !globalThis[globalThis._native_n_func_name]) return null;
				return globalThis[globalThis._native_n_func_name](')" +
					   js_escape(n_param) + R"(');
			} catch (e) { return null; }
		})()
	)";
	auto res = js_->evaluate_and_get(code);
	if (res.has_value() && res.value() != "null" && !res.value().empty())
		return res.value();
	return n_param;
}

std::string NativeJsSolver::extract_iife_body(const std::string &player_code) {
	static const boost::regex re(R"(\((function\s*\(.+?\)\s*\{))");
	boost::smatch m;
	if (boost::regex_search(player_code, m, re)) {
		size_t start_pos = m.position(1);
		bool in_quote = false;
		char quote_char = 0;
		bool in_regex = false;
		int depth = 0;
		bool found_start = false;
		for (size_t i = start_pos; i < player_code.size(); ++i) {
			char c = player_code[i];
			char prev = (i > 0) ? player_code[i - 1] : 0;
			bool is_escaped = false;
			if (prev == '\\') {
				size_t backslash_count = 0;
				for (size_t j = i - 1; j < i; --j) {
					if (player_code[j] == '\\')
						backslash_count++;
					else
						break;
				}
				if (backslash_count % 2 != 0) is_escaped = true;
			}
			if ((c == '"' || c == '\'' || c == '`') && !is_escaped &&
				!in_regex) {
				if (!in_quote) {
					in_quote = true;
					quote_char = c;
				} else if (c == quote_char) {
					in_quote = false;
				}
			} else if (c == '/' && !in_quote && !is_escaped) {
				if (in_regex) {
					in_regex = false;
				} else {
					char p = 0;
					for (size_t j = i - 1; j != (size_t)-1; --j) {
						if (!isspace(player_code[j])) {
							p = player_code[j];
							break;
						}
					}
					if (strchr("(=,[!:&|?{};", p) || p == 0) {
						in_regex = true;
					}
				}
			} else if (c == '{' && !in_quote && !in_regex) {
				depth++;
				found_start = true;
			} else if (c == '}' && !in_quote && !in_regex) {
				depth--;
				if (found_start && depth == 0) {
					return player_code.substr(
						player_code.find('{', start_pos) + 1,
						i - player_code.find('{', start_pos) - 1);
				}
			}
		}
	}
	return "";
}

std::vector<std::string> NativeJsSolver::split_toplevel_statements(
	const std::string &code) {
	std::vector<std::string> stmts;
	std::string current;
	current.reserve(1024);
	bool in_quote = false;
	char quote_char = 0;
	bool in_regex = false;
	int brace_depth = 0;
	int paren_depth = 0;

	for (size_t i = 0; i < code.size(); ++i) {
		char c = code[i];
		char prev = (i > 0) ? code[i - 1] : 0;
		bool is_escaped = false;
		if (prev == '\\') {
			size_t backslash_count = 0;
			for (size_t j = i - 1; j < i; --j) {
				if (code[j] == '\\')
					backslash_count++;
				else
					break;
			}
			if (backslash_count % 2 != 0) is_escaped = true;
		}
		if ((c == '"' || c == '\'' || c == '`') && !is_escaped && !in_regex) {
			if (!in_quote) {
				in_quote = true;
				quote_char = c;
			} else if (c == quote_char) {
				in_quote = false;
			}
		} else if (c == '/' && !in_quote && !is_escaped) {
			if (in_regex) {
				in_regex = false;
			} else {
				char p = 0;
				for (size_t j = i - 1; j != (size_t)-1; --j) {
					if (!isspace(code[j])) {
						p = code[j];
						break;
					}
				}
				if (strchr("(=,[!:&|?{};", p) || p == 0) { in_regex = true; }
			}
		} else if (c == '{' && !in_quote && !in_regex) {
			brace_depth++;
		} else if (c == '}' && !in_quote && !in_regex) {
			brace_depth--;
		} else if (c == '(' && !in_quote && !in_regex) {
			paren_depth++;
		} else if (c == ')' && !in_quote && !in_regex) {
			paren_depth--;
		}

		current += c;
		if (c == ';' && !in_quote && !in_regex && brace_depth == 0 &&
			paren_depth == 0) {
			stmts.push_back(current);
			current.clear();
			current.reserve(1024);
		}
	}
	if (!current.empty()) stmts.push_back(current);
	return stmts;
}

std::string NativeJsSolver::filter_statements(
	const std::vector<std::string> &statements) {
	std::string result;
	for (const auto &stmt : statements) {
		bool keep = true;
		if (stmt.rfind("try", 0) == 0)
			keep = false;
		else if (stmt.rfind("if", 0) == 0)
			keep = false;
		else if (stmt.rfind("return", 0) == 0)
			keep = false;
		else if (stmt.rfind("throw", 0) == 0)
			keep = false;
		else if (stmt.rfind("while", 0) == 0)
			keep = false;
		else if (stmt.rfind("do", 0) == 0)
			keep = false;
		else if (stmt.rfind("switch", 0) == 0)
			keep = false;
		else if (stmt.rfind("break", 0) == 0)
			keep = false;
		else if (stmt.rfind("continue", 0) == 0)
			keep = false;

		if (stmt.rfind("for", 0) == 0) keep = true;

		if (keep) result += stmt + "\n";
	}
	return result;
}

std::string NativeJsSolver::js_escape(const std::string &str) {
	std::string result;
	for (char c : str) {
		if (c == '"')
			result += "\\\"";
		else if (c == '\\')
			result += "\\\\";
		else if (c == '\n')
			result += "\\n";
		else if (c == '\r')
			result += "\\r";
		else
			result += c;
	}
	return result;
}

std::string NativeJsSolver::find_sig_function(const std::string &) {
	return "";
}
std::string NativeJsSolver::find_n_function(const std::string &) { return ""; }
bool NativeJsSolver::load_ejs_scripts() { return true; }
std::string NativeJsSolver::build_preprocess_call(const std::string &) const {
	return "";
}
bool NativeJsSolver::parse_jsc_result(const std::string &) { return true; }
bool NativeJsSolver::preprocess_player_native(const std::string &) {
	return true;
}
bool NativeJsSolver::load_preprocessed_script(const std::string &) {
	return true;
}

}  // namespace ytdlpp
