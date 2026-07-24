#ifndef MCP_SERVER_PLUGIN_API_H
#define MCP_SERVER_PLUGIN_API_H

#ifdef _WIN32
#define PLUGIN_API __declspec(dllexport)
#else
#define PLUGIN_API __attribute__((visibility("default")))
#endif

#define MCP_PLUGIN_API_VERSION 2

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* id;            /* Stable id: plugin_id.tool_name. */
    const char* name;          /* Model-safe name: plugin_id__tool_name. */
    const char* description;
    const char* inputSchema;
    const char* outputSchema;
} PluginTool;

typedef struct {
    int (*GetApiVersion)();
    const char* (*GetId)();
    const char* (*GetName)();
    const char* (*GetVersion)();
    int (*Initialize)();
    void (*Shutdown)();
    int (*GetToolCount)();
    const PluginTool* (*GetTool)(int index);
    /* Arguments and context are UTF-8 JSON objects. The returned buffer must
       be released through FreeResult, never by the host directly. */
    char* (*HandleToolCall)(const char* toolId,
                            const char* arguments,
                            const char* context);
    void (*FreeResult)(char* result);
} PluginAPI;

PLUGIN_API PluginAPI* CreatePlugin();
PLUGIN_API void DestroyPlugin(PluginAPI* api);

#ifdef __cplusplus
}
#endif

#endif
