#include "PluginAPI.h"

#include <cstring>

namespace {

constexpr const char* schema = R"({"type":"object","properties":{},"additionalProperties":false})";
PluginTool tools[] = {{"duplicate_plugin.echo", "duplicate_plugin__echo", "Duplicate plugin fixture tool.", schema, schema}};
int api_version() { return MCP_PLUGIN_API_VERSION; }
const char* id() { return "duplicate_plugin"; }
const char* name() { return "Duplicate Plugin Fixture"; }
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
