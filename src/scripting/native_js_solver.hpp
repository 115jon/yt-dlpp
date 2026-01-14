#pragma once

#include <optional>
#include <string>
#include <vector>

namespace ytdlpp::scripting {
class JsEngine;
}  // namespace ytdlpp::scripting

namespace ytdlpp {

/**
 * JavaScript solver using yt-dlp's EJS solver scripts.
 *
 * This implementation uses the same approach as yt-dlp:
 * 1. Load meriyah + astring (bundled in yt.solver.lib.min.js)
 * 2. Load the solver logic (yt.solver.core.min.js)
 * 3. Call jsc() to preprocess player and extract n/sig functions
 * 4. Execute challenges through the extracted functions
 *
 * The EJS scripts handle all JavaScript parsing via meriyah, which properly
 * handles ES6+ syntax, template literals, regex, etc.
 */
class NativeJsSolver {
   public:
	explicit NativeJsSolver(scripting::JsEngine *js);

	/**
	 * Initialize with the EJS solver scripts.
	 * Must be called before any other methods.
	 */
	bool init();

	/**
	 * Parse the player script and extract decipher functions.
	 */
	bool load_player(const std::string &player_code);

	/**
	 * Solve a signature challenge.
	 */
	std::string solve_sig(const std::string &encrypted_sig) const;

	/**
	 * Solve an n-parameter challenge.
	 */
	std::string solve_n(const std::string &n_param) const;

	/**
	 * Check if the solver is ready.
	 */
	[[nodiscard]] bool is_ready() const { return ready_; }

   private:
	scripting::JsEngine *js_;
	bool initialized_{false};
	bool ready_{false};

	// Load the EJS solver scripts into JS engine
	bool load_ejs_scripts();

	// Build the jsc() call to preprocess a player
	std::string build_preprocess_call(const std::string &player_code) const;

	// Parse the jsc() result and extract function names
	bool parse_jsc_result(const std::string &json_result);

	// Escape a string for safe embedding in JS code
	static std::string js_escape(const std::string &str);

	// --- Native C++ Parsing Helpers (Robust Fallback) ---
	std::string extract_iife_body(const std::string &player_code);
	std::vector<std::string> split_toplevel_statements(const std::string &code);
	std::string filter_statements(const std::vector<std::string> &statements);
	std::string find_n_function(const std::string &code);
	std::string find_sig_function(const std::string &code);
	bool preprocess_player_native(const std::string &player_code);
	bool load_preprocessed_script(const std::string &script_code);
};

}  // namespace ytdlpp
