#include "PluginAPI.h"
#include "PluginSupport.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>

using vx::plugin::json;

namespace {

namespace fs = std::filesystem;
constexpr std::uintmax_t kMaxFileBytes = 1024 * 1024;
constexpr std::uintmax_t kMaxWorkspaceBytes = 100 * 1024 * 1024;
constexpr std::size_t kMaxEntries = 1000;
std::mutex fs_mutex;

constexpr const char* kOutput = R"({"type":"object","properties":{"ok":{"type":"boolean"},"result":{"type":"object"},"error":{"type":"object","properties":{"code":{"type":"string"},"message":{"type":"string"}},"required":["code","message"],"additionalProperties":false}},"required":["ok"],"additionalProperties":false})";
PluginTool tools[] = {
    {"workspace_fs.stat_path", "workspace_fs__stat_path", "Return metadata for a workspace path.", R"({"type":"object","properties":{"path":{"type":"string","minLength":1,"maxLength":4096}},"required":["path"],"additionalProperties":false})", kOutput},
    {"workspace_fs.list_directory", "workspace_fs__list_directory", "List a workspace directory without following symlinks.", R"({"type":"object","properties":{"path":{"type":"string","minLength":1,"maxLength":4096},"recursive":{"type":"boolean"}},"required":["path"],"additionalProperties":false})", kOutput},
    {"workspace_fs.read_file", "workspace_fs__read_file", "Read a UTF-8 text file from the session workspace.", R"({"type":"object","properties":{"path":{"type":"string","minLength":1,"maxLength":4096},"max_bytes":{"type":"integer","minimum":1,"maximum":1048576}},"required":["path"],"additionalProperties":false})", kOutput},
    {"workspace_fs.write_file", "workspace_fs__write_file", "Create or atomically overwrite a UTF-8 text file.", R"({"type":"object","properties":{"path":{"type":"string","minLength":1,"maxLength":4096},"content":{"type":"string","maxLength":1048576},"overwrite":{"type":"boolean"}},"required":["path","content"],"additionalProperties":false})", kOutput},
    {"workspace_fs.replace_text", "workspace_fs__replace_text", "Replace exact text in an existing workspace file.", R"({"type":"object","properties":{"path":{"type":"string","minLength":1,"maxLength":4096},"old_text":{"type":"string","minLength":1,"maxLength":1048576},"new_text":{"type":"string","maxLength":1048576},"replace_all":{"type":"boolean"}},"required":["path","old_text","new_text"],"additionalProperties":false})", kOutput},
    {"workspace_fs.create_directory", "workspace_fs__create_directory", "Create a workspace directory.", R"({"type":"object","properties":{"path":{"type":"string","minLength":1,"maxLength":4096},"recursive":{"type":"boolean"}},"required":["path"],"additionalProperties":false})", kOutput},
    {"workspace_fs.move_path", "workspace_fs__move_path", "Move or rename a file or directory inside the workspace.", R"({"type":"object","properties":{"source":{"type":"string","minLength":1,"maxLength":4096},"destination":{"type":"string","minLength":1,"maxLength":4096},"overwrite":{"type":"boolean"}},"required":["source","destination"],"additionalProperties":false})", kOutput},
    {"workspace_fs.delete_file", "workspace_fs__delete_file", "Delete one regular workspace file.", R"({"type":"object","properties":{"path":{"type":"string","minLength":1,"maxLength":4096}},"required":["path"],"additionalProperties":false})", kOutput},
    {"workspace_fs.delete_directory", "workspace_fs__delete_directory", "Delete a workspace directory, optionally recursively.", R"({"type":"object","properties":{"path":{"type":"string","minLength":1,"maxLength":4096},"recursive":{"type":"boolean"}},"required":["path"],"additionalProperties":false})", kOutput}
};

bool beneath(const fs::path& root, const fs::path& value) {
    auto root_it = root.begin();
    auto value_it = value.begin();
    for (; root_it != root.end(); ++root_it, ++value_it) {
        if (value_it == value.end() || *root_it != *value_it) return false;
    }
    return true;
}

fs::path resolve_path(const json& context, const std::string& raw, bool allow_root = false) {
    if (raw.empty() || raw.find('\0') != std::string::npos) throw std::runtime_error("path is empty or contains NUL");
    if (raw.find('\\') != std::string::npos ||
        (raw.size() >= 2 && std::isalpha(static_cast<unsigned char>(raw[0])) && raw[1] == ':')) {
        throw std::runtime_error("Windows absolute or backslash paths are not allowed");
    }
    const fs::path relative(raw);
    if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) throw std::runtime_error("absolute paths are not allowed");
    for (const auto& part : relative) if (part == "..") throw std::runtime_error("parent traversal is not allowed");
    if (!allow_root && relative.lexically_normal() == fs::path(".")) {
        throw std::runtime_error("workspace root operations are not allowed");
    }
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(context.at("workspace_root").get<std::string>(), ec);
    if (ec) throw std::runtime_error("invalid workspace root");
    const fs::path candidate = (root / relative).lexically_normal();
    if (!beneath(root, candidate) ||
        (!allow_root && fs::weakly_canonical(candidate, ec) == root)) {
        throw std::runtime_error("path escapes or targets workspace root");
    }
    fs::path cursor = root;
    for (const auto& part : candidate.lexically_relative(root)) {
        cursor /= part;
        if (fs::exists(cursor, ec) && fs::is_symlink(fs::symlink_status(cursor, ec))) throw std::runtime_error("symlink traversal is not allowed");
    }
    const auto canonical_parent = fs::weakly_canonical(candidate.parent_path(), ec);
    if (ec || !beneath(root, canonical_parent)) throw std::runtime_error("path parent escapes workspace");
    return candidate;
}

std::uintmax_t workspace_size(const fs::path& root) {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec)) {
        if (it->is_symlink(ec)) { it.disable_recursion_pending(); continue; }
        if (it->is_regular_file(ec)) total += it->file_size(ec);
        if (total > kMaxWorkspaceBytes) break;
    }
    return total;
}

std::string read_limited(const fs::path& path, std::size_t limit) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open file");
    std::string content;
    content.resize(limit + 1);
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    content.resize(static_cast<std::size_t>(input.gcount()));
    if (content.size() > limit) throw std::runtime_error("file exceeds requested or configured limit");
    return content;
}

void atomic_write(const fs::path& path, const std::string& content, bool overwrite) {
    std::error_code ec;
    if (!fs::is_directory(path.parent_path(), ec)) throw std::runtime_error("parent directory does not exist");
    if (fs::exists(path, ec) && !overwrite) throw std::runtime_error("destination already exists; set overwrite=true");
    if (fs::exists(path, ec) && !fs::is_regular_file(path, ec)) throw std::runtime_error("destination is not a regular file");
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path temporary = path.parent_path() / ("." + path.filename().string() + ".tmp-" + std::to_string(nonce));
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("cannot create temporary file");
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!output) { fs::remove(temporary, ec); throw std::runtime_error("file write failed"); }
    }
#ifdef _WIN32
    if (overwrite && fs::exists(path, ec)) fs::remove(path, ec);
#endif
    fs::rename(temporary, path, ec);
    if (ec) { fs::remove(temporary); throw std::runtime_error("atomic rename failed: " + ec.message()); }
}

json path_metadata(const fs::path& root, const fs::path& path) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec || !fs::exists(status)) throw std::runtime_error("path does not exist");
    std::string type = fs::is_regular_file(status) ? "file" : fs::is_directory(status) ? "directory" : fs::is_symlink(status) ? "symlink" : "other";
    json result = {{"path", path.lexically_relative(root).generic_string()}, {"type", type}};
    if (type == "file") result["size"] = fs::file_size(path, ec);
    return result;
}

char* handle(const char* tool_id, const char* raw_arguments, const char* raw_context) {
    try {
        const json arguments = json::parse(raw_arguments ? raw_arguments : "{}");
        const json context = json::parse(raw_context ? raw_context : "{}");
        const fs::path root = fs::weakly_canonical(context.at("workspace_root").get<std::string>());
        const std::string id = tool_id ? tool_id : "";
        std::lock_guard<std::mutex> lock(fs_mutex);

        if (id == "workspace_fs.stat_path") {
            return vx::plugin::copy_result(vx::plugin::success(path_metadata(root, resolve_path(context, arguments.at("path").get<std::string>(), true))));
        }
        if (id == "workspace_fs.list_directory") {
            const auto directory = resolve_path(context, arguments.at("path").get<std::string>(), true);
            if (!fs::is_directory(directory)) throw std::runtime_error("path is not a directory");
            const bool recursive = arguments.value("recursive", false);
            json entries = json::array();
            std::error_code ec;
            if (recursive) {
                for (fs::recursive_directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec)) {
                    entries.push_back(path_metadata(root, it->path()));
                    if (entries.size() >= kMaxEntries) break;
                    if (it->is_symlink(ec)) it.disable_recursion_pending();
                }
            } else {
                for (fs::directory_iterator it(directory, ec), end; it != end && !ec; it.increment(ec)) {
                    entries.push_back(path_metadata(root, it->path()));
                    if (entries.size() >= kMaxEntries) break;
                }
            }
            return vx::plugin::copy_result(vx::plugin::success({{"entries", entries}, {"truncated", entries.size() >= kMaxEntries}}));
        }
        if (id == "workspace_fs.read_file") {
            const auto path = resolve_path(context, arguments.at("path").get<std::string>());
            if (!fs::is_regular_file(path)) throw std::runtime_error("path is not a regular file");
            const auto maximum = static_cast<std::size_t>(arguments.value("max_bytes", static_cast<int>(kMaxFileBytes)));
            return vx::plugin::copy_result(vx::plugin::success({{"path", path.lexically_relative(root).generic_string()}, {"content", read_limited(path, maximum)}}));
        }
        if (id == "workspace_fs.write_file") {
            const auto path = resolve_path(context, arguments.at("path").get<std::string>());
            const std::string content = arguments.at("content").get<std::string>();
            if (content.size() > kMaxFileBytes) throw std::runtime_error("file exceeds 1 MiB limit");
            const std::uintmax_t old_size = fs::is_regular_file(path) ? fs::file_size(path) : 0;
            if (workspace_size(root) - old_size + content.size() > kMaxWorkspaceBytes) throw std::runtime_error("session workspace exceeds 100 MiB quota");
            atomic_write(path, content, arguments.value("overwrite", false));
            return vx::plugin::copy_result(vx::plugin::success({{"path", path.lexically_relative(root).generic_string()}, {"size", content.size()}}));
        }
        if (id == "workspace_fs.replace_text") {
            const auto path = resolve_path(context, arguments.at("path").get<std::string>());
            std::string content = read_limited(path, kMaxFileBytes);
            const std::uintmax_t old_size = content.size();
            const std::string old_text = arguments.at("old_text").get<std::string>();
            const std::string new_text = arguments.at("new_text").get<std::string>();
            std::size_t count = 0, position = 0;
            while ((position = content.find(old_text, position)) != std::string::npos) {
                content.replace(position, old_text.size(), new_text);
                position += new_text.size();
                ++count;
                if (!arguments.value("replace_all", false)) break;
            }
            if (!count) throw std::runtime_error("old_text was not found");
            if (content.size() > kMaxFileBytes) throw std::runtime_error("updated file exceeds 1 MiB limit");
            if (workspace_size(root) - old_size + content.size() > kMaxWorkspaceBytes) {
                throw std::runtime_error("session workspace exceeds 100 MiB quota");
            }
            atomic_write(path, content, true);
            return vx::plugin::copy_result(vx::plugin::success({{"replacements", count}, {"size", content.size()}}));
        }
        if (id == "workspace_fs.create_directory") {
            const auto path = resolve_path(context, arguments.at("path").get<std::string>());
            std::error_code ec;
            const bool created = arguments.value("recursive", true) ? fs::create_directories(path, ec) : fs::create_directory(path, ec);
            if (ec) throw std::runtime_error("cannot create directory: " + ec.message());
            return vx::plugin::copy_result(vx::plugin::success({{"path", path.lexically_relative(root).generic_string()}, {"created", created}}));
        }
        if (id == "workspace_fs.move_path") {
            const auto source = resolve_path(context, arguments.at("source").get<std::string>());
            const auto destination = resolve_path(context, arguments.at("destination").get<std::string>());
            std::error_code ec;
            if (!fs::exists(source, ec)) throw std::runtime_error("source does not exist");
            if (fs::exists(destination, ec)) {
                if (!arguments.value("overwrite", false) || fs::is_directory(destination, ec)) throw std::runtime_error("destination already exists");
#ifdef _WIN32
                fs::remove(destination, ec);
#endif
            }
            fs::rename(source, destination, ec);
            if (ec) throw std::runtime_error("move failed: " + ec.message());
            return vx::plugin::copy_result(vx::plugin::success({{"source", arguments.at("source")}, {"destination", arguments.at("destination")}}));
        }
        if (id == "workspace_fs.delete_file") {
            const auto path = resolve_path(context, arguments.at("path").get<std::string>());
            if (!fs::is_regular_file(path)) throw std::runtime_error("path is not a regular file");
            if (!fs::remove(path)) throw std::runtime_error("file was not deleted");
            return vx::plugin::copy_result(vx::plugin::success({{"deleted", true}}));
        }
        if (id == "workspace_fs.delete_directory") {
            const auto path = resolve_path(context, arguments.at("path").get<std::string>());
            if (!fs::is_directory(path)) throw std::runtime_error("path is not a directory");
            std::error_code ec;
            const auto removed = arguments.value("recursive", false) ? fs::remove_all(path, ec) : (fs::remove(path, ec) ? 1 : 0);
            if (ec) throw std::runtime_error("directory delete failed: " + ec.message());
            return vx::plugin::copy_result(vx::plugin::success({{"deleted_entries", removed}}));
        }
        return vx::plugin::copy_result(vx::plugin::failure("UNKNOWN_TOOL", "unknown workspace_fs tool"));
    } catch (const std::exception& exception) {
        return vx::plugin::copy_result(vx::plugin::failure("FILESYSTEM_ERROR", exception.what()));
    }
}

int api_version() { return MCP_PLUGIN_API_VERSION; }
const char* plugin_id() { return "workspace_fs"; }
const char* plugin_name() { return "Session Workspace Filesystem"; }
const char* plugin_version() { return "1.0.0"; }
int initialize() { return 1; }
void shutdown() {}
int tool_count() { return static_cast<int>(sizeof(tools) / sizeof(tools[0])); }
const PluginTool* get_tool(int index) { return index >= 0 && index < tool_count() ? &tools[index] : nullptr; }

}  // namespace

extern "C" PLUGIN_API PluginAPI* CreatePlugin() {
    return new PluginAPI{api_version, plugin_id, plugin_name, plugin_version, initialize,
                         shutdown, tool_count, get_tool, handle, vx::plugin::free_result};
}
extern "C" PLUGIN_API void DestroyPlugin(PluginAPI* api) { delete api; }
