#pragma once

#include <string>

#include "../scripting/quickjs_engine.hpp"

namespace ytdlpp::youtube {

class SigDecipherer {
   public:
	explicit SigDecipherer(scripting::JsEngine &js);

	// Extracts and loads the relevant decipher functions from the player code
	// into the JS engine. Returns true if successful.
	bool load_functions(const std::string &player_code);

	// Deciphers a signature.
	std::string decipher_signature(const std::string &signature);

	// Transforms the 'n' parameter.
	std::string transform_n(const std::string &n);

   private:
	scripting::JsEngine &js_;
	std::string sig_func_name_;
	std::string n_func_name_;

	std::string extract_function(const std::string &code,
								 const std::string &func_name);
	std::string extract_helper_object(const std::string &code,
									  const std::string &object_name);
};

}  // namespace ytdlpp::youtube
