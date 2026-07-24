#include "PluginAPI.h"

#include <cstring>

namespace {

PluginTool tools[] = {{
    "invalid_schema.echo", "invalid_schema__echo", "Tool with an invalid fixture schema.",
    R"({"type":"object","properties":[]})",
    R"({"type":"object","properties":{"ok":{"type":"boolean"}},"required":["ok"]})"
}};
int api_version() { return MCP_PLUGIN_API_VERSION; }
const char* id() { return "invalid_schema"; }
const char* name() { return "Invalid Schema Fixture"; }
const char* version() { return "1.0.0"; }
int initialize() { return 1; }
void shutdown() {}
int count() { return 1; }
const PluginTool* tool(int index) { return index == 0 ? &tools[0] : nullptr; }
char* call(const char*, const char*, const char*) {
    const char* value = R"({"ok":true})";
    auto* output = new char[std::strlen(value) + 1];
    std::memcpy(output, value, std::strlen(value) + 1);
    return output;
}
void release(char* value) { delete[] value; }

}  // namespace

extern "C" PLUGIN_API PluginAPI* CreatePlugin() {
    return new PluginAPI{api_version, id, name, version, initialize, shutdown,
                         count, tool, call, release};
}
extern "C" PLUGIN_API void DestroyPlugin(PluginAPI* api) { delete api; }
