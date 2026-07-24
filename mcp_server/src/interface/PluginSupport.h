#pragma once

#include "json.hpp"

#include <cstring>
#include <string>

namespace vx::plugin {

using json = nlohmann::json;

inline char* copy_result(const json& value) {
    const std::string text = value.dump();
    auto* result = new char[text.size() + 1];
    std::memcpy(result, text.c_str(), text.size() + 1);
    return result;
}

inline void free_result(char* value) { delete[] value; }

inline json success(json value = json::object()) {
    return {{"ok", true}, {"result", std::move(value)}};
}

inline json failure(const std::string& code, const std::string& message) {
    return {{"ok", false}, {"error", {{"code", code}, {"message", message}}}};
}

inline std::string state_key(const json& context) {
    return context.value("session_id", "") + ":" + context.value("operation_id", "");
}

}  // namespace vx::plugin
