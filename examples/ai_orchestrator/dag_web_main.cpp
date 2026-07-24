#include "ai_orchestrator/dag_runtime.hpp"
#include "ai_orchestrator/session_manager.hpp"

#include "httplib.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using ai_orchestrator::dag::AgentDefinition;
using ai_orchestrator::dag::DagRuntime;
using ai_orchestrator::dag::QwenChatModel;
using ai_orchestrator::dag::RunOptions;
using ai_orchestrator::dag::SessionManager;
using ai_orchestrator::dag::json;

namespace {

std::string environment(const char* name, const std::string& fallback = {}) {
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}

std::string endpoint_from_environment() {
    std::string endpoint = environment("QWEN_API_URL");
    if (endpoint.empty()) endpoint = environment("QWEN_BASE_URL");
    if (endpoint.empty()) {
        endpoint = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
    } else {
        while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
        const std::string suffix = "/chat/completions";
        // 完整 Chat Completions 地址不应被重复追加路径。
        if (endpoint.size() < suffix.size() ||
            endpoint.compare(endpoint.size() - suffix.size(), suffix.size(), suffix) != 0) {
            endpoint += suffix;
        }
    }
    return endpoint;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read static file: " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void cors(httplib::Response& response) {
    response.set_header("Access-Control-Allow-Origin", "*");
    response.set_header("Access-Control-Allow-Headers", "Content-Type, X-Session-ID");
}

void send_json(httplib::Response& response, const json& value, int status = 200) {
    response.status = status;
    cors(response);
    response.set_content(value.dump(), "application/json; charset=utf-8");
}

json error_json(const std::string& message) {
    return {{"detail", message}};
}

std::string request_session(const httplib::Request& request) {
    return request.get_header_value("X-Session-ID");
}

std::vector<std::string> string_array(const json& value) {
    std::vector<std::string> result;
    if (!value.is_array()) return result;
    for (const auto& item : value) if (item.is_string()) result.push_back(item.get<std::string>());
    return result;
}

json public_tool_json(const agent_rpc::mcp::ToolInfo& tool) {
    return {{"name", tool.name},
            {"tool_id", tool.tool_id},
            {"plugin_id", tool.plugin_id},
            {"description", tool.description}};
}

std::vector<AgentDefinition> parse_agents(const json& value) {
    std::vector<AgentDefinition> result;
    if (!value.is_array()) return result;
    for (const auto& item : value) {
        if (!item.is_object()) continue;
        AgentDefinition agent;
        agent.id = item.value("id", "");
        agent.name = item.value("name", agent.id);
        agent.role = item.value("role", "Complete a specialized part of the user task.");
        agent.icon = item.value("icon", "A");
        agent.private_materials = string_array(item.value("reference_materials", json::array()));
        if (!agent.id.empty()) result.push_back(std::move(agent));
    }
    return result;
}

std::string base64_decode(const std::string& input) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    int buffer = 0;
    int bits = -8;
    for (unsigned char ch : input) {
        if (ch == '=') break;
        const char* found = std::find(alphabet, alphabet + 64, ch);
        if (found == alphabet + 64) continue;
        buffer = (buffer << 6) + static_cast<int>(found - alphabet);
        bits += 6;
        if (bits >= 0) {
            output.push_back(static_cast<char>((buffer >> bits) & 0xff));
            bits -= 8;
        }
    }
    return output;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string api_key = environment("QWEN_API_KEY");
    if (api_key.empty()) {
        std::cerr << "QWEN_API_KEY is required before starting ai_orchestrator" << std::endl;
        return 2;
    }

    int port = 8000;
    if (argc > 1) port = std::stoi(argv[1]);
    if (!environment("PORT").empty()) port = std::stoi(environment("PORT"));

    std::filesystem::path static_dir = environment("WEB_STATIC_DIR");
    if (static_dir.empty()) static_dir = AGENT_DAG_WEB_ROOT;
    std::filesystem::path log_directory = environment("DAG_LOG_DIR");
    if (log_directory.empty()) log_directory = std::filesystem::path(AGENT_DAG_PROJECT_ROOT) / "log";
    std::filesystem::path sessions_directory = environment("DAG_SESSIONS_DIR");
    if (sessions_directory.empty()) sessions_directory = std::filesystem::path(AGENT_DAG_PROJECT_ROOT) / "sessions";
    std::filesystem::path mcp_server_path = environment("MCP_SERVER_PATH");
    if (mcp_server_path.empty()) mcp_server_path = std::filesystem::path(AGENT_DAG_BUILD_ROOT) / "mcp_server/mcp_server";
    std::filesystem::path plugins_directory = environment("MCP_PLUGINS_DIR");
    if (plugins_directory.empty()) plugins_directory = std::filesystem::path(AGENT_DAG_BUILD_ROOT) / "mcp_server/plugins";
    std::filesystem::path tool_config = environment("TOOL_CONFIG_PATH");
    if (tool_config.empty()) tool_config = AGENT_DAG_TOOL_CONFIG;

    try {
        SessionManager sessions(sessions_directory);
        std::filesystem::create_directories(log_directory / "mcp");
        agent_rpc::mcp::MCPAgentConfig mcp_config;
        mcp_config.enable_mcp = true;
        mcp_config.mcp_server_path = mcp_server_path.string();
        mcp_config.max_retry_count = 0;
        mcp_config.mcp_args = {"-p", plugins_directory.string(),
                               "-c", tool_config.string(),
                               "--sessions-root", sessions.root().string(),
                               "-l", (log_directory / "mcp").string()};
        auto tools = std::make_shared<agent_rpc::mcp::MCPAgentIntegration>();
        if (!tools->initialize(mcp_config)) {
            throw std::runtime_error("cannot initialize required MCP tool runtime");
        }
        auto model = std::make_shared<QwenChatModel>(api_key,
                                                       environment("QWEN_MODEL", "qwen-plus"),
                                                       endpoint_from_environment());
        DagRuntime runtime(model, tools, log_directory);
        httplib::Server server;
        server.set_error_handler([&](const httplib::Request& request, httplib::Response& response) {
            if (!response.body.empty()) return;
            if (response.status != 404) {
                cors(response);
                return;
            }
            if (request.path == "/") {
                response.status = 200;
                cors(response);
                response.set_content(read_file(static_dir / "static/index.html"), "text/html; charset=utf-8");
                return;
            }
            send_json(response, error_json("not found"), 404);
        });
        server.set_pre_routing_handler([&](const httplib::Request& request, httplib::Response& response) {
            if (request.path == "/") {
                try {
                    response.status = 200;
                    cors(response);
                    response.set_content(read_file(static_dir / "static/index.html"), "text/html; charset=utf-8");
                } catch (const std::exception& error) {
                    send_json(response, error_json(error.what()), 500);
                }
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
        server.Options("/.*", [](const httplib::Request&, httplib::Response& response) {
            cors(response);
            response.status = 204;
        });

        const auto index_path = static_dir / "static/index.html";
        server.Get("/", [&](const httplib::Request&, httplib::Response& response) {
            try { response.status = 200; cors(response); response.set_content(read_file(index_path), "text/html; charset=utf-8"); }
            catch (const std::exception& error) { send_json(response, error_json(error.what()), 500); }
        });
        const auto static_assets = static_dir / "static";
        if (!server.set_mount_point("/static", static_assets.string().c_str())) {
            throw std::runtime_error("cannot mount static directory: " + static_assets.string());
        }

        server.Get("/health", [tools](const httplib::Request&, httplib::Response& response) {
            send_json(response, {{"status", tools->isAvailable() ? "ok" : "error"},
                                 {"service", "agent-dag-orchestrator"},
                                 {"tools", tools->getStatusDescription()}},
                      tools->isAvailable() ? 200 : 503);
        });
        server.Get("/api/settings", [](const httplib::Request&, httplib::Response& response) {
            send_json(response, {{"default_planner", "dashscope"}, {"model", environment("QWEN_MODEL", "qwen-plus")},
                                 {"server_api_key_configured", true}});
        });
        server.Post("/api/sessions", [&](const httplib::Request&, httplib::Response& response,
                                          const httplib::ContentReader&) {
            try {
                const auto session_id = sessions.create();
                send_json(response, {{"session_id", session_id}});
            } catch (const std::exception& error) {
                send_json(response, error_json(error.what()), 500);
            }
        });
        server.Get(R"(/api/sessions/([a-f0-9]{32}))", [&](const httplib::Request& request, httplib::Response& response) {
            const auto session_id = request.matches[1].str();
            if (!sessions.resume(session_id)) send_json(response, error_json("Unknown session_id"), 404);
            else send_json(response, {{"session_id", session_id}, {"status", "ready"}});
        });
        server.Get("/api/agents", [&](const httplib::Request&, httplib::Response& response) {
            json result = json::array();
            for (const auto& agent : runtime.agents()) result.push_back(agent.public_json());
            send_json(response, {{"agents", result}});
        });
        server.Get("/api/agents/tools", [&](const httplib::Request& request, httplib::Response& response) {
            const auto session_id = request_session(request);
            if (!sessions.exists(session_id)) {
                send_json(response, error_json("Valid X-Session-ID is required"), 401);
                return;
            }
            if (!tools->isAvailable()) {
                send_json(response, error_json("MCP tool runtime is unavailable"), 503);
                return;
            }

            json agents = json::object();
            json executor_tools = json::array();
            for (const auto& agent : runtime.agents()) {
                agent_rpc::mcp::ToolExecutionContext context;
                context.session_id = session_id;
                context.operation_id = "agent_tools_" + agent.id;
                context.mode_id = "dag_team";
                context.actor_kind = "executor";
                context.actor_id = agent.id;
                context.trusted_data = "{}";

                json available = json::array();
                for (const auto& tool : tools->getAvailableTools(context)) {
                    available.push_back(public_tool_json(tool));
                }
                agents[agent.id] = available;
                if (executor_tools.empty()) executor_tools = available;
            }
            send_json(response, {{"agents", agents},
                                 {"executor_tools", executor_tools},
                                 {"tools", executor_tools}});
        });
        server.Post("/api/agents/draft", [&](const httplib::Request& request, httplib::Response& response) {
            try {
                const auto session_id = request_session(request);
                if (!sessions.exists(session_id)) { send_json(response, error_json("Valid X-Session-ID is required"), 401); return; }
                const auto body = json::parse(request.body);
                const std::string task = body.value("user_input", "");
                const int max_agents = body.value("max_dynamic_agents", 6);
                const auto references = string_array(body.value("reference_materials", json::array()));
                const auto agents = runtime.draft_agents(session_id, task, max_agents, references);
                json result = json::array();
                for (const auto& agent : agents) result.push_back(agent.public_json());
                send_json(response, {{"agents", result}, {"blueprints", result}});
            } catch (const std::exception& error) { send_json(response, error_json(error.what()), 422); }
        });
        server.Post("/api/materials/parse", [&](const httplib::Request& request, httplib::Response& response) {
            try {
                if (!sessions.exists(request_session(request))) { send_json(response, error_json("Valid X-Session-ID is required"), 401); return; }
                const auto body = json::parse(request.body);
                const std::string filename = body.value("filename", "material.txt");
                const auto dot = filename.rfind('.');
                const std::string extension = dot == std::string::npos ? "" : filename.substr(dot + 1);
                if (extension != "txt" && extension != "md" && extension != "markdown") {
                    send_json(response, error_json("C++ runtime supports MD/TXT materials only"), 400);
                    return;
                }
                send_json(response, {{"filename", filename}, {"text", base64_decode(body.value("content_base64", ""))}});
            } catch (const std::exception& error) { send_json(response, error_json(error.what()), 400); }
        });
        server.Post("/api/runs", [&](const httplib::Request& request, httplib::Response& response) {
            try {
                const auto session_id = request_session(request);
                if (!sessions.exists(session_id)) { send_json(response, error_json("Valid X-Session-ID is required"), 401); return; }
                const auto body = json::parse(request.body);
                RunOptions options;
                options.session_id = session_id;
                options.user_input = body.value("user_input", "");
                options.auto_agents = body.value("agent_mode", "manual") == "auto";
                options.max_dynamic_agents = body.value("max_dynamic_agents", 6);
                options.max_agent_rounds = body.value("max_agent_rounds", 5);
                options.reference_materials = string_array(body.value("reference_materials", json::array()));
                options.agents = parse_agents(body.value("agents", json::array()));
                if (options.agents.empty() && body.contains("agent_ids") && body["agent_ids"].is_array()) {
                    const auto requested = string_array(body["agent_ids"]);
                    for (const auto& agent : runtime.agents()) {
                        if (std::find(requested.begin(), requested.end(), agent.id) != requested.end()) options.agents.push_back(agent);
                    }
                }
                const auto private_refs = body.value("agent_reference_materials", json::object());
                if (private_refs.is_object()) {
                    for (auto& item : private_refs.items()) options.agent_reference_materials[item.key()] = string_array(item.value());
                    for (auto& agent : options.agents) if (private_refs.contains(agent.id)) agent.private_materials = string_array(private_refs[agent.id]);
                }
                const auto run_id = runtime.create_run(std::move(options));
                send_json(response, {{"run_id", run_id}});
            } catch (const std::exception& error) { send_json(response, error_json(error.what()), 400); }
        });
        server.Get(R"(/api/runs/([^/]+))", [&](const httplib::Request& request, httplib::Response& response) {
            const auto session_id = request_session(request);
            if (!sessions.exists(session_id)) { send_json(response, error_json("Valid X-Session-ID is required"), 401); return; }
            const auto result = runtime.snapshot(session_id, request.matches[1]);
            if (result.is_null() || result.empty()) send_json(response, error_json("Unknown run_id"), 404);
            else send_json(response, result);
        });
        server.Post(R"(/api/runs/([^/]+)/agents/([^/]+)/retry)", [&](const httplib::Request& request, httplib::Response& response) {
            try {
                const auto session_id = request_session(request);
                if (!sessions.exists(session_id)) { send_json(response, error_json("Valid X-Session-ID is required"), 401); return; }
                const auto body = json::parse(request.body.empty() ? "{}" : request.body);
                const auto run_id = request.matches[1].str();
                const auto agent_id = request.matches[2].str();
                const bool accepted = runtime.retry_agent(run_id, session_id, agent_id,
                    body.value("edited_prior", ""), body.value("user_feedback", ""));
                if (!accepted) send_json(response, error_json("Unknown run_id or agent_id"), 404);
                else send_json(response, {{"run_id", run_id}, {"agent_id", agent_id}, {"status", "retry_started"}});
            } catch (const std::exception& error) { send_json(response, error_json(error.what()), 400); }
        });

        // cpp-httplib evaluates mounted files before regular handlers. Keep a
        // regex fallback for the bare root so the browser always receives the
        // console even when the requested path is normalized by a proxy.
        server.Get(R"(/.*)", [&](const httplib::Request& request, httplib::Response& response) {
            if (request.path != "/") return;
            try { response.status = 200; cors(response); response.set_content(read_file(index_path), "text/html; charset=utf-8"); }
            catch (const std::exception& error) { send_json(response, error_json(error.what()), 500); }
        });

        std::cout << "[DAG] Web service listening on http://127.0.0.1:" << port << std::endl;
        std::cout << "[DAG] Static directory: " << static_dir << std::endl;
        if (!server.listen("127.0.0.1", port)) {
            std::cerr << "[DAG] failed to listen on port " << port << std::endl;
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "[DAG] startup failed: " << error.what() << std::endl;
        return 1;
    }
    return 0;
}
