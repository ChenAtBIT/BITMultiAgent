//  The MIT License
//
//  Copyright (C) 2025 Giuseppe Mastrangelo
//
//  Permission is hereby granted, free of charge, to any person obtaining
//  a copy of this software and associated documentation files (the
//  'Software'), to deal in the Software without restriction, including
//  without limitation the rights to use, copy, modify, merge, publish,
//  distribute, sublicense, and/or sell copies of the Software, and to
//  permit persons to whom the Software is furnished to do so, subject to
//  the following conditions:
//
//  The above copyright notice and this permission notice shall be
//  included in all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED 'AS IS', WITHOUT WARRANTY OF ANY KIND,
//  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
//  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
//  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
//  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
//  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
//  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//

#include "version.h"
#include "httplib.h"
#include "popl.hpp"
#include "StdioTransport.h"
#include "SseTransport.h"
#include "server/Server.h"
#include "aixlog.hpp"
#include "loader/PluginsLoader.h"
#include "json.hpp"
#include "utils/MCPBuilder.h"
#include <csignal>

using namespace popl;

/**
 * @brief 全局 MCP 服务器实例，供信号处理和插件通知回调复用。
 */
std::shared_ptr<vx::mcp::Server> server;

/**
 * @brief 保存通知发送过程中需要共享的同步对象。
 */
struct NotificationState {
    /// 串行化插件到客户端的通知发送，避免多线程并发访问服务器。
    std::mutex serverNotificationMutex;
};
NotificationState notificationState;

/**
 * @brief 处理 Ctrl+C 中断信号并安全停止服务器。
 * @param s 捕获到的信号编号，当前主要用于处理 SIGINT。
 */
void stop_handler(sig_atomic_t s) {
    // 当前实现只注册了 SIGINT，这里显式忽略参数值本身。
    (void)s;
    std::cout <<"Stopping server..." << std::endl;
    if (server && server->IsValid()) {
        // 当服务器仍处于有效状态时，先触发停止流程再退出进程。
        server->Stop();
    }
    std::cout << "done." << std::endl;
    exit(0);
}

/**
 * @brief 将插件侧产生的通知转发给当前连接的 MCP 客户端。
 * @param pluginName 发送通知的插件名称。
 * @param notification 插件生成的通知内容。
 */
void ClientNotificationCallbackImpl(const char* pluginName, const char* notification) {
    // 通过互斥锁确保多插件并发发送通知时不会发生交叉写入。
    std::lock_guard<std::mutex> lock(notificationState.serverNotificationMutex);
    if (server && server->IsValid()) {
        server->SendNotification(pluginName, notification);
    }
}

/**
 * @brief 程序主入口，负责初始化传输层、日志、插件与 MCP 服务。
 * @param argc 命令行参数个数。
 * @param argv 命令行参数数组。
 * @return 成功返回 0，参数解析失败时返回 -1。
 */
int main(int argc, char **argv) {
    std::string name;
    std::string plugins_directory;
    std::string logs_directory;
    bool verbose;

    std::shared_ptr<vx::ITransport> transport;
    auto loader = std::make_shared<vx::mcp::PluginsLoader>();
    server = std::make_shared<vx::mcp::Server>();

    //============================================================================================
    // 注册 Ctrl+C 信号处理函数，确保终止时能够主动停止服务器。
    //============================================================================================
    signal(SIGINT, stop_handler);

    //============================================================================================
    // 配置命令行参数，允许外部指定服务名、插件目录、日志目录和传输模式。
    //============================================================================================
    OptionParser op("Allowed options");
    auto help_option = op.add<Switch>("", "help", "produce help message");
    auto name_option = op.add<Value<std::string>>("n", "name", "the name of the server", "mcp-server"); // 默认服务器名称为 "mcp-server"，用户可通过 -n 参数覆盖。
    auto plugins_directory_option = op.add<Value<std::string>>("p", "plugins", "the directory where to load the plugins", "./plugins");
    auto logs_directory_option = op.add<Value<std::string>>("l", "logs", "the directory where to store the logs", "./logs");
    auto verbose_option = op.add<Value<bool>>("v", "verbose", "enable verbose", verbose);
    auto use_sse_server = op.add<Switch>("s", "sse", "start as sse server");
    name_option->assign_to(&name);
    plugins_directory_option->assign_to(&plugins_directory);
    logs_directory_option->assign_to(&logs_directory);
    verbose_option->assign_to(&verbose);

    //============================================================================================
    // 解析命令行参数；若用户请求帮助或参数非法，则直接返回。
    //============================================================================================
    try {
        op.parse(argc, argv);
        if (help_option->count() == 1) {
            std::cout << op << std::endl;
            return 0;
        }
    } catch (const popl::invalid_option& e) {
        std::cerr << "Invalid Option Exception: " << e.what() << std::endl;
        return -1;
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return -1;
    }

    //============================================================================================
    // 根据命令行参数选择传输层：SSE 用于网络服务，默认使用标准输入输出模式。
    //============================================================================================
    if (use_sse_server->is_set()) {
        transport = std::make_shared<vx::transport::SSE>();
    } else {
        transport = std::make_shared<vx::transport::Stdio>();
    }

    //============================================================================================
    // 初始化日志系统，并为日志文件名追加 UTC 时间戳，便于区分不同运行实例。
    //============================================================================================
    // 将当前时间格式化为 ISO 8601 风格字符串，作为日志文件名的一部分。
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t_now), "%Y-%m-%dT%H-%M-%S");
    std::string iso_date = ss.str();

    // 组合日志目录和时间戳，生成本次启动对应的日志文件路径。
    std::string logFilename = logs_directory + "/mcp-server_" + iso_date + ".log";
    auto sink_file = std::make_shared<AixLog::SinkFile>(AixLog::Severity::trace, logFilename);
    AixLog::Log::init({sink_file});

    //============================================================================================
    // 输出启动横幅和运行信息，方便定位当前版本、传输方式和监听端口。
    //============================================================================================
    LOG(INFO) << " __  __  _____ _____        _____ ______ _______      ________ _____  " << std::endl;
    LOG(INFO) << "|  \\/  |/ ____|  __ \\      / ____|  ____|  __ \\ \\    / /  ____|  __ \\ " << std::endl;
    LOG(INFO) << "| \\  / | |    | |__) |____| (___ | |__  | |__) \\ \\  / /| |__  | |__) |" << std::endl;
    LOG(INFO) << "| |\\/| | |    |  ___/______\\___ \\|  __| |  _  / \\ \\/ / |  __| |  _  / " << std::endl;
    LOG(INFO) << "| |  | | |____| |          ____) | |____| | \\ \\  \\  /  | |____| | \\ \\ " << std::endl;
    LOG(INFO) << "|_|  |_|\\_____|_|         |_____/|______|_|  \\_\\  \\/   |______|_|  \\_\\" << std::endl;
    LOG(INFO) << "Starting mcp-server v" << PROJECT_VERSION << " (transport: " << transport->GetName() << " v" << transport->GetVersion() << ") on port: " << transport->GetPort() << std::endl;
    LOG(INFO) << "Press Ctrl+C to exit." << std::endl;

    //============================================================================================
    // 从指定插件目录加载全部插件，后续的 tools、prompts、resources 均由插件提供。
    //============================================================================================
    if (loader->LoadPlugins(plugins_directory)) {
        LOG(INFO) << "Successfully loaded plugins" << std::endl;
    }

    //============================================================================================
    // 为每个插件注入通知系统，使插件能够主动向 MCP 客户端推送消息。
    //============================================================================================
    for (auto& plugin : loader->GetPlugins()) {
        plugin.instance->notifications = new NotificationSystem();
        plugin.instance->notifications->SendToClient = ClientNotificationCallbackImpl;
    }

    //============================================================================================
    // 配置服务器基础信息，并注册 MCP 标准方法对应的处理回调。
    //============================================================================================
    server->Name(name);
    server->VerboseLevel(verbose ? 1 : 0);
    server->OverrideCallback("tools/list", [&loader](const json& request) {
        nlohmann::ordered_json response = MCPBuilder::Response(request);
        response["result"]["tools"] = json::array();

        // 遍历工具类插件，将每个插件暴露的工具元信息转换为 MCP 返回结构。
        for (const auto& plugin : loader->GetPlugins()) {
            if (plugin.instance->GetType() == PLUGIN_TYPE_TOOLS) {
                for (int i = 0; i < plugin.instance->GetToolCount(); i++) {
                    nlohmann::ordered_json tool;
                    auto pluginTool = plugin.instance->GetTool(i);
                    tool["name"] = pluginTool->name;
                    tool["description"] = pluginTool->description;
                    tool["inputSchema"] = nlohmann::json::parse(pluginTool->inputSchema);
                    response["result"]["tools"].push_back(tool);
                }
            }
        }

        return response;
    });
    server->OverrideCallback("tools/call", [&loader](const json& request) {
        nlohmann::ordered_json response = MCPBuilder::Response(request);

        // 插件返回的是动态分配的 C 字符串，这里在解析完成后负责释放。
        char* res_ptr = nullptr;

        // 在所有工具插件中查找目标工具，并将完整请求转交给对应插件处理。
        for (const auto& plugin : loader->GetPlugins()) {
            if (plugin.instance->GetType() == PLUGIN_TYPE_TOOLS) {
                for (int i = 0; i < plugin.instance->GetToolCount(); i++) {
                    auto pluginTool = plugin.instance->GetTool(i);
                    if (pluginTool->name == request["params"]["name"]) {
                        res_ptr = plugin.instance->HandleRequest(request.dump().c_str());
                        if (res_ptr) {
                            try {
                                // 插件返回的内容应当是合法 JSON；解析后补充统一的错误标记。
                                response["result"] = json::parse(res_ptr);
                                response["result"]["isError"] = false;
                            } catch (const json::parse_error& e) {
                                // 当插件返回格式错误时，向客户端返回统一的文本错误信息。
                                response["result"]["isError"] = true;
                                response["result"]["content"] = json::array();
                                response["result"]["content"].push_back({{"type", "text"}, {"text", "Plugin returned malformed data."}});
                            }
                            // 释放插件分配的返回缓冲区，避免内存泄漏。
                            delete[] res_ptr;
                        } else {
                            LOG(ERROR) << "Plugin " << pluginTool->name << " returned nullptr." << std::endl;
                        }
                        return response;
                    }
                }
            }
        }

        // 未找到匹配的工具时，按 MCP 内容格式构造错误响应。
        response["result"]["isError"] = true;
        response["result"]["content"] = json::array();
        response["result"]["content"].push_back({
            {"type", "text"},
            {"text", "Tool not found: " + request["params"]["name"].get<std::string>()}
        });
        return response;
    });
    server->OverrideCallback("prompts/list", [&loader](const json& request) {
        nlohmann::ordered_json response = MCPBuilder::Response(request);
        response["result"]["prompts"] = json::array();

        // 聚合所有提示词插件暴露的 Prompt 描述信息。
        for (const auto& plugin : loader->GetPlugins()) {
            if (plugin.instance->GetType() == PLUGIN_TYPE_PROMPTS) {
                for (int i = 0; i < plugin.instance->GetPromptCount(); i++) {
                    nlohmann::ordered_json prompt;
                    auto pluginPrompt = plugin.instance->GetPrompt(i);
                    prompt["name"] = pluginPrompt->name;
                    prompt["description"] = pluginPrompt->description;
                    prompt["arguments"] = nlohmann::json::parse(pluginPrompt->arguments);
                    response["result"]["prompts"].push_back(prompt);
                }
            }
        }

        return response;
    });
    server->OverrideCallback("prompts/get", [&loader](const json& request) {
        nlohmann::ordered_json response = MCPBuilder::Response(request);

        // 插件返回的是动态分配的内容，需要在当前回调中接管释放责任。
        char* res_ptr = nullptr;

        // 查找名称匹配的 Prompt，并将原始请求交给插件生成具体内容。
        for (const auto& plugin : loader->GetPlugins()) {
            if (plugin.instance->GetType() == PLUGIN_TYPE_PROMPTS) {
                for (int i = 0; i < plugin.instance->GetPromptCount(); i++) {
                    auto pluginPrompt = plugin.instance->GetPrompt(i);
                    if (pluginPrompt->name == request["params"]["name"]) {
                        res_ptr = plugin.instance->HandleRequest(request.dump().c_str());
                        if (res_ptr) {
                            try {
                                response["result"] = json::parse(res_ptr);
                            } catch (const json::parse_error& e) {
                                LOG(ERROR) << "Plugin " << pluginPrompt->name << " returned malformed data." << std::endl;
                                // TODO: 这里尚未向客户端返回结构化错误，仅记录日志。
                            }
                            // 释放插件返回的堆内存，避免重复请求后积累泄漏。
                            delete[] res_ptr;
                        }
                    }
                    return response;
                }
            }
        }

        return response;
    });
    server->OverrideCallback("resources/list", [&loader](const json& request) {
        nlohmann::ordered_json response = MCPBuilder::Response(request);
        response["result"]["resources"] = json::array();

        // 收集资源类插件暴露的资源清单，供客户端后续按 URI 读取。
        for (const auto& plugin : loader->GetPlugins()) {
            if (plugin.instance->GetType() == PLUGIN_TYPE_RESOURCES) {
                for (int i = 0; i < plugin.instance->GetResourceCount(); i++) {
                    nlohmann::ordered_json resource;
                    auto pluginResource = plugin.instance->GetResource(i);
                    resource["name"] = pluginResource->name;
                    resource["description"] = pluginResource->description;
                    resource["uri"] = pluginResource->uri;
                    resource["mimeType"] = pluginResource->mime;
                    response["result"]["resources"].push_back(resource);
                }
            }
        }

        return response;
    });
    server->OverrideCallback("resources/read", [&loader](const json& request) {
        nlohmann::ordered_json response = MCPBuilder::Response(request);

        // 插件返回的内容为堆内存，需要在当前函数中完成释放。
        char* res_ptr = nullptr;

        // 根据客户端请求的 URI 查找对应资源，并由插件完成实际读取。
        for (const auto& plugin : loader->GetPlugins()) {
            if (plugin.instance->GetType() == PLUGIN_TYPE_RESOURCES) {
                for (int i = 0; i < plugin.instance->GetResourceCount(); i++) {
                    auto pluginResource = plugin.instance->GetResource(i);
                    if (pluginResource->uri == request["params"]["uri"]) {
                        res_ptr = plugin.instance->HandleRequest(request.dump().c_str());
                        if (res_ptr) {
                            try {
                                response["result"] = json::parse(res_ptr);
                            } catch (const json::parse_error& e) {
                                LOG(ERROR) << "Plugin " << pluginResource->name << " returned malformed data." << std::endl;
                                // TODO: 这里尚未向客户端返回结构化错误，仅记录日志。
                            }
                            // 释放插件返回的堆内存，避免资源读取路径发生泄漏。
                            delete[] res_ptr;
                        }
                    }
                }
            }
        }

        return response;
    });

    // 启动传输层并进入服务循环，后续请求将由以上回调接管处理。
    server->Connect(transport);

    return 0;
}
