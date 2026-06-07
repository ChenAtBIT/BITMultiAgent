/**
 * @file MeetingFileTools.cpp
 * @brief MCP 会议文件工具插件 - 提供纪要生成所需的文件读写能力
 */

#include "PluginAPI.h"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

/**
 * @brief MCP 工具定义
 *
 * 工具说明里显式写出“会议纪要”“Markdown”“会议记录”等关键词，
 * 这样 Minutes Agent 在启用 RAG-MCP 时也更容易检索到对应能力。
 */
static PluginTool tools[] = {
    {
        "read_text_file",
        "读取本地文本文件内容，适合读取会议记录、会议转写稿、议程、日志或 Markdown 草稿；支持按行分段读取长文件。",
        R"({
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "要读取的本地文件路径，支持绝对路径和相对路径。"
                },
                "start_line": {
                    "type": "integer",
                    "minimum": 1,
                    "description": "从第几行开始读取，默认 1。"
                },
                "max_lines": {
                    "type": "integer",
                    "minimum": 1,
                    "maximum": 2000,
                    "description": "本次最多读取多少行，默认 400。"
                },
                "max_chars": {
                    "type": "integer",
                    "minimum": 256,
                    "maximum": 50000,
                    "description": "本次最多返回多少个字符，默认 12000。"
                }
            },
            "required": ["path"]
        })"
    },
    {
        "write_text_file",
        "将生成好的会议纪要或其他文本内容写入本地文件，适合写出 Markdown 纪要结果，可自动创建父目录。",
        R"({
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "要写入的目标文件路径。"
                },
                "content": {
                    "type": "string",
                    "description": "要写入的完整文本内容，例如 Markdown 纪要。"
                },
                "overwrite": {
                    "type": "boolean",
                    "description": "是否覆盖已有文件，默认 true。"
                },
                "create_directories": {
                    "type": "boolean",
                    "description": "是否自动创建父目录，默认 true。"
                }
            },
            "required": ["path", "content"]
        })"
    },
    {
        "derive_markdown_output_path",
        "根据会议源文件路径推导 Markdown 输出路径；当用户没有明确给出纪要保存路径时，可用它生成默认的 .md 文件名。",
        R"({
            "type": "object",
            "properties": {
                "source_path": {
                    "type": "string",
                    "description": "会议源文件路径。"
                },
                "output_dir": {
                    "type": "string",
                    "description": "可选输出目录；如果为空，则默认使用源文件所在目录。"
                },
                "suffix": {
                    "type": "string",
                    "description": "附加在源文件名后的后缀，默认 _minutes。"
                },
                "filename": {
                    "type": "string",
                    "description": "可选自定义文件名；如果未提供，则自动根据源文件名推导。"
                }
            },
            "required": ["source_path"]
        })"
    }
};

/**
 * @brief 在堆上构造插件响应字符串
 * @param payload 要返回的 JSON 对象
 * @return 供宿主释放的 C 字符串
 */
char* make_response(const json& payload) {
    const std::string result = payload.dump();
    char* buffer = new char[result.size() + 1];
#ifdef _WIN32
    strcpy_s(buffer, result.size() + 1, result.c_str());
#else
    std::strcpy(buffer, result.c_str());
#endif
    return buffer;
}

/**
 * @brief 统一返回文本结果
 * @param text 结果文本
 * @param is_error 是否表示错误
 * @return MCP 结果 JSON
 */
json make_text_result(const std::string& text, bool is_error) {
    json response;
    response["content"] = json::array();
    response["content"].push_back({
        {"type", "text"},
        {"text", text}
    });
    response["isError"] = is_error;
    return response;
}

/**
 * @brief 读取字符串参数
 * @param args 参数对象
 * @param key 字段名
 * @param default_value 默认值
 * @return 解析后的字符串
 */
std::string get_string_arg(const json& args,
                           const std::string& key,
                           const std::string& default_value) {
    if (!args.contains(key) || args[key].is_null()) {
        return default_value;
    }
    if (args[key].is_string()) {
        return args[key].get<std::string>();
    }
    return default_value;
}

/**
 * @brief 读取整数参数
 * @param args 参数对象
 * @param key 字段名
 * @param default_value 默认值
 * @return 解析后的整数
 */
int get_int_arg(const json& args, const std::string& key, int default_value) {
    if (!args.contains(key) || args[key].is_null()) {
        return default_value;
    }
    if (args[key].is_number_integer()) {
        return args[key].get<int>();
    }
    if (args[key].is_number()) {
        return static_cast<int>(args[key].get<double>());
    }
    return default_value;
}

/**
 * @brief 读取布尔参数
 * @param args 参数对象
 * @param key 字段名
 * @param default_value 默认值
 * @return 解析后的布尔值
 */
bool get_bool_arg(const json& args, const std::string& key, bool default_value) {
    if (!args.contains(key) || args[key].is_null()) {
        return default_value;
    }
    if (args[key].is_boolean()) {
        return args[key].get<bool>();
    }
    return default_value;
}

/**
 * @brief 去掉字符串首尾空白
 * @param text 原始字符串
 * @return 去空白后的结果
 */
std::string trim(const std::string& text) {
    size_t start = 0;
    while (start < text.size() &&
           std::isspace(static_cast<unsigned char>(text[start]))) {
        start++;
    }

    size_t end = text.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        end--;
    }

    return text.substr(start, end - start);
}

/**
 * @brief 将输入路径规范化为绝对路径
 * @param raw_path 用户传入路径
 * @return 规范化后的路径
 */
std::filesystem::path normalize_path(const std::string& raw_path) {
    if (raw_path.empty()) {
        throw std::runtime_error("文件路径不能为空");
    }

    const std::filesystem::path path = raw_path;
    if (path.is_absolute()) {
        return path.lexically_normal();
    }

    return std::filesystem::absolute(path).lexically_normal();
}

/**
 * @brief 检查文件是否疑似二进制内容
 * @param path 文件路径
 * @return true 表示检测到 NUL 字节
 */
bool looks_like_binary_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("无法打开文件: " + path.string());
    }

    std::vector<char> buffer(4096, '\0');
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize bytes_read = input.gcount();

    for (std::streamsize index = 0; index < bytes_read; ++index) {
        if (buffer[static_cast<size_t>(index)] == '\0') {
            return true;
        }
    }
    return false;
}

/**
 * @brief 读取文本文件的指定片段
 * @param args 工具参数
 * @return JSON 字符串结果
 */
std::string handle_read_text_file(const json& args) {
    const std::filesystem::path path = normalize_path(
        get_string_arg(args, "path", ""));
    const int start_line = std::max(1, get_int_arg(args, "start_line", 1));
    const int max_lines = std::max(1, get_int_arg(args, "max_lines", 400));
    const int max_chars = std::max(256, get_int_arg(args, "max_chars", 12000));

    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("文件不存在: " + path.string());
    }
    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error("目标不是普通文件: " + path.string());
    }
    if (looks_like_binary_file(path)) {
        throw std::runtime_error("当前工具只支持文本文件，检测到二进制内容: " + path.string());
    }

    std::ifstream input(path);
    if (!input.is_open()) {
        throw std::runtime_error("无法读取文件: " + path.string());
    }

    std::ostringstream collected;
    std::string line;
    int line_number = 0;
    int collected_lines = 0;
    int end_line = 0;
    int next_start_line = 0;
    bool truncated = false;
    bool collecting = true;
    size_t current_size = 0;

    while (std::getline(input, line)) {
        line_number++;
        if (line_number < start_line) {
            continue;
        }

        if (!collecting) {
            continue;
        }

        if (collected_lines >= max_lines) {
            truncated = true;
            next_start_line = line_number;
            collecting = false;
            continue;
        }

        std::string chunk = line;
        if (collected_lines > 0) {
            chunk = "\n" + chunk;
        }

        // 超过字符预算时，保留已读取片段，并告诉调用方从当前行继续。
        if (static_cast<int>(current_size + chunk.size()) > max_chars &&
            collected_lines > 0) {
            truncated = true;
            next_start_line = line_number;
            collecting = false;
            continue;
        }

        // 如果首行本身就超过上限，则也至少返回这一行，避免死循环。
        if (static_cast<int>(current_size + chunk.size()) > max_chars &&
            collected_lines == 0) {
            chunk.resize(static_cast<size_t>(max_chars));
            truncated = true;
            next_start_line = line_number + 1;
        }

        collected << chunk;
        current_size += chunk.size();
        collected_lines++;
        end_line = line_number;

        if (truncated) {
            collecting = false;
        }
    }

    json result;
    result["success"] = true;
    result["path"] = path.string();
    result["start_line"] = start_line;
    result["end_line"] = end_line;
    result["next_start_line"] = next_start_line;
    result["returned_line_count"] = collected_lines;
    result["total_lines"] = line_number;
    result["truncated"] = truncated;
    result["content"] = collected.str();
    return result.dump();
}

/**
 * @brief 将文本内容写入文件
 * @param args 工具参数
 * @return JSON 字符串结果
 */
std::string handle_write_text_file(const json& args) {
    const std::filesystem::path path = normalize_path(
        get_string_arg(args, "path", ""));
    const std::string content = get_string_arg(args, "content", "");
    const bool overwrite = get_bool_arg(args, "overwrite", true);
    const bool create_directories = get_bool_arg(args, "create_directories", true);
    const bool file_existed = std::filesystem::exists(path);

    bool created_directories = false;
    if (path.has_parent_path() && create_directories) {
        // 为纪要输出目录自动补齐父目录，避免因目录不存在导致流程中断。
        created_directories = std::filesystem::create_directories(path.parent_path());
    }

    if (file_existed && !overwrite) {
        throw std::runtime_error("目标文件已存在且 overwrite=false: " + path.string());
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error("无法写入文件: " + path.string());
    }

    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.close();

    json result;
    result["success"] = true;
    result["path"] = path.string();
    result["bytes_written"] = content.size();
    result["created_directories"] = created_directories;
    result["overwritten"] = file_existed;
    return result.dump();
}

/**
 * @brief 根据源文件路径推导 Markdown 输出路径
 * @param args 工具参数
 * @return JSON 字符串结果
 */
std::string handle_derive_markdown_output_path(const json& args) {
    const std::filesystem::path source_path = normalize_path(
        get_string_arg(args, "source_path", ""));
    const std::string output_dir_arg = trim(get_string_arg(args, "output_dir", ""));
    const std::string suffix = get_string_arg(args, "suffix", "_minutes");
    std::string filename = trim(get_string_arg(args, "filename", ""));

    std::filesystem::path output_dir;
    if (!output_dir_arg.empty()) {
        output_dir = normalize_path(output_dir_arg);
    } else if (source_path.has_parent_path()) {
        output_dir = source_path.parent_path();
    } else {
        output_dir = std::filesystem::current_path();
    }

    if (filename.empty()) {
        filename = source_path.stem().string() + suffix + ".md";
    } else if (std::filesystem::path(filename).extension().empty()) {
        filename += ".md";
    }

    const std::filesystem::path output_path = (output_dir / filename).lexically_normal();

    json result;
    result["success"] = true;
    result["source_path"] = source_path.string();
    result["output_path"] = output_path.string();
    result["output_dir"] = output_dir.string();
    return result.dump();
}

}  // namespace

const char* GetNameImpl() {
    return "meeting-file-tools";
}

const char* GetVersionImpl() {
    return "1.0.0";
}

PluginType GetTypeImpl() {
    return PLUGIN_TYPE_TOOLS;
}

int InitializeImpl() {
    return 1;
}

char* HandleRequestImpl(const char* req) {
    try {
        const json request = json::parse(req);
        const std::string tool_name = request.at("params").at("name").get<std::string>();
        const json arguments = request.at("params").value("arguments", json::object());

        if (tool_name == "read_text_file") {
            return make_response(make_text_result(handle_read_text_file(arguments), false));
        }
        if (tool_name == "write_text_file") {
            return make_response(make_text_result(handle_write_text_file(arguments), false));
        }
        if (tool_name == "derive_markdown_output_path") {
            return make_response(make_text_result(
                handle_derive_markdown_output_path(arguments), false));
        }

        return make_response(make_text_result("未知工具: " + tool_name, true));
    } catch (const std::exception& ex) {
        return make_response(make_text_result("错误: " + std::string(ex.what()), true));
    }
}

void ShutdownImpl() {
}

int GetToolCountImpl() {
    return static_cast<int>(sizeof(tools) / sizeof(tools[0]));
}

const PluginTool* GetToolImpl(int index) {
    if (index < 0 || index >= GetToolCountImpl()) {
        return nullptr;
    }
    return &tools[index];
}

static PluginAPI plugin = {
    GetNameImpl,
    GetVersionImpl,
    GetTypeImpl,
    InitializeImpl,
    HandleRequestImpl,
    ShutdownImpl,
    GetToolCountImpl,
    GetToolImpl,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr
};

extern "C" PLUGIN_API PluginAPI* CreatePlugin() {
    return &plugin;
}

extern "C" PLUGIN_API void DestroyPlugin(PluginAPI*) {
}
