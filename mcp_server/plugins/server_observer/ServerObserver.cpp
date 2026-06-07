/**
 * @file ServerObserver.cpp
 * @brief MCP Server Observer Plugin - 服务器状态观测工具
 */

#include "PluginAPI.h"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <vector>

#include <sys/statvfs.h>

using json = nlohmann::json;

namespace {

struct CpuSample {
    long long total = 0;
    long long idle = 0;
};

struct DiskSnapshot {
    std::string path = "/";
    double used_percent = 0.0;
    double free_gb = 0.0;
    double total_gb = 0.0;
    double inode_used_percent = 0.0;
    std::string status = "healthy";
};

struct NetworkCounters {
    unsigned long long rx_bytes = 0;
    unsigned long long tx_bytes = 0;
    unsigned long long rx_errors = 0;
    unsigned long long tx_errors = 0;
    unsigned long long rx_drops = 0;
    unsigned long long tx_drops = 0;
};

struct CommandResult {
    std::string output;
    int exit_code = 0;
};

/**
 * @brief 去掉首尾空白字符
 * @param text 原始文本
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
 * @brief 读取 JSON 字符串参数
 * @param args 参数对象
 * @param key 参数名
 * @param default_value 默认值
 * @return 参数值
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
 * @brief 读取 JSON 整数参数
 * @param args 参数对象
 * @param key 参数名
 * @param default_value 默认值
 * @return 参数值
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
 * @brief 生成当前 UTC 时间戳
 * @return ISO-8601 格式时间字符串
 */
std::string now_iso8601() {
    const std::time_t now = std::time(nullptr);
    std::tm utc_time{};
#ifdef _WIN32
    gmtime_s(&utc_time, &now);
#else
    gmtime_r(&now, &utc_time);
#endif

    std::ostringstream oss;
    oss << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

/**
 * @brief 根据使用率生成健康等级
 * @param value 当前数值
 * @param warning_threshold 预警阈值
 * @param critical_threshold 严重阈值
 * @return healthy、warning 或 critical
 */
std::string level_from_threshold(double value,
                                 double warning_threshold,
                                 double critical_threshold) {
    if (value >= critical_threshold) {
        return "critical";
    }
    if (value >= warning_threshold) {
        return "warning";
    }
    return "healthy";
}

/**
 * @brief 将字节数换算为 GB
 * @param bytes 字节数
 * @return GB 数值
 */
double bytes_to_gb(unsigned long long bytes) {
    return static_cast<double>(bytes) / 1024.0 / 1024.0 / 1024.0;
}

/**
 * @brief 构造 MCP 文本响应
 * @param payload 文本内容
 * @param is_error 是否错误
 * @return 插件返回的 JSON 字符串
 */
std::string make_text_response(const std::string& payload, bool is_error) {
    json content;
    content["type"] = "text";
    content["text"] = payload;

    json response;
    response["content"] = json::array();
    response["content"].push_back(content);
    response["isError"] = is_error;
    return response.dump();
}

/**
 * @brief 执行本地诊断命令并收集输出
 * @param command 诊断命令
 * @return 标准输出与退出码
 */
CommandResult run_command_capture(const std::string& command) {
    std::FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("无法执行命令: " + command);
    }

    std::string output;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output.append(buffer);
    }

    const int raw_status = pclose(pipe);
    int exit_code = raw_status;
#ifdef WIFEXITED
    if (WIFEXITED(raw_status)) {
        exit_code = WEXITSTATUS(raw_status);
    }
#endif

    return {output, exit_code};
}

/**
 * @brief 校验并规范化主机名/地址
 * @param host 原始目标
 * @return 清洗后的目标
 */
std::string sanitize_host_or_throw(const std::string& host) {
    const std::string trimmed = trim(host);
    if (trimmed.empty()) {
        throw std::runtime_error("host 不能为空");
    }

    for (unsigned char ch : trimmed) {
        if (std::isalnum(ch) ||
            ch == '.' ||
            ch == '-' ||
            ch == '_' ||
            ch == ':' ||
            ch == '%') {
            continue;
        }
        throw std::runtime_error("host 包含不安全字符: " + trimmed);
    }

    return trimmed;
}

/**
 * @brief 规范化协议过滤参数
 * @param protocol 原始协议名
 * @return tcp、udp 或 all
 */
std::string normalize_protocol_filter(const std::string& protocol) {
    std::string trimmed = trim(protocol);
    std::transform(trimmed.begin(), trimmed.end(), trimmed.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    if (trimmed.empty() || trimmed == "all") {
        return "all";
    }
    if (trimmed == "tcp" || trimmed == "udp") {
        return trimmed;
    }
    throw std::runtime_error("protocol 只支持 tcp、udp 或 all");
}

/**
 * @brief 拆分地址与端口
 * @param endpoint 形如 0.0.0.0:5000 或 [::1]:323 的端点字符串
 * @return 地址与端口文本
 */
std::pair<std::string, std::string> split_endpoint(const std::string& endpoint) {
    const size_t colon_pos = endpoint.rfind(':');
    if (colon_pos == std::string::npos) {
        return {endpoint, ""};
    }

    std::string address = endpoint.substr(0, colon_pos);
    if (!address.empty() && address.front() == '[' && address.back() == ']') {
        address = address.substr(1, address.size() - 2);
    }
    return {address, endpoint.substr(colon_pos + 1)};
}

/**
 * @brief 解析 ss 命令输出中的进程信息
 * @param owners_text ss 行尾的 users:(...) 文本
 * @return 进程数组
 */
json parse_socket_owners(const std::string& owners_text) {
    static const std::regex owner_regex(
        R"owner(\("([^"]+)",pid=([0-9]+),fd=([0-9]+)\))owner");
    json owners = json::array();

    std::sregex_iterator iter(owners_text.begin(), owners_text.end(), owner_regex);
    std::sregex_iterator end;
    for (; iter != end; ++iter) {
        owners.push_back({
            {"process_name", (*iter)[1].str()},
            {"pid", std::stoi((*iter)[2].str())},
            {"fd", std::stoi((*iter)[3].str())}
        });
    }

    return owners;
}

/**
 * @brief 读取 CPU 采样值
 * @return CPU 总时间与空闲时间
 */
CpuSample read_cpu_sample() {
    std::ifstream input("/proc/stat");
    if (!input.is_open()) {
        throw std::runtime_error("无法读取 /proc/stat");
    }

    std::string label;
    long long user = 0;
    long long nice = 0;
    long long system = 0;
    long long idle = 0;
    long long iowait = 0;
    long long irq = 0;
    long long softirq = 0;
    long long steal = 0;
    input >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;

    CpuSample sample;
    sample.idle = idle + iowait;
    sample.total = user + nice + system + idle + iowait + irq + softirq + steal;
    return sample;
}

/**
 * @brief 读取 load average
 * @return load average JSON
 */
json read_load_average() {
    std::ifstream input("/proc/loadavg");
    if (!input.is_open()) {
        return {{"load1", 0.0}, {"load5", 0.0}, {"load15", 0.0}};
    }

    double load1 = 0.0;
    double load5 = 0.0;
    double load15 = 0.0;
    input >> load1 >> load5 >> load15;
    return {
        {"load1", load1},
        {"load5", load5},
        {"load15", load15}
    };
}

/**
 * @brief 采样 CPU 快照
 * @param server_id 服务器标识
 * @param sample_ms 采样窗口
 * @return CPU 观测结果
 */
json collect_cpu_snapshot(const std::string& server_id, int sample_ms) {
    const int safe_sample_ms = std::clamp(sample_ms, 200, 5000);
    const CpuSample first = read_cpu_sample();
    std::this_thread::sleep_for(std::chrono::milliseconds(safe_sample_ms));
    const CpuSample second = read_cpu_sample();

    const long long total_delta = second.total - first.total;
    const long long idle_delta = second.idle - first.idle;
    double usage_percent = 0.0;
    if (total_delta > 0) {
        usage_percent =
            static_cast<double>(total_delta - idle_delta) * 100.0 / static_cast<double>(total_delta);
    }

    const std::string status = level_from_threshold(usage_percent, 70.0, 85.0);
    std::string recommendation = "CPU 状态正常，保持持续观察即可。";
    if (status == "warning") {
        recommendation = "CPU 使用率偏高，建议结合 top 进程继续排查是否有异常任务。";
    } else if (status == "critical") {
        recommendation = "CPU 使用率很高，建议立即查看 top 进程、批处理任务和并发作业。";
    }

    json result = {
        {"tool", "get_cpu_snapshot"},
        {"server_id", server_id},
        {"sample_ms", safe_sample_ms},
        {"usage_percent", std::round(usage_percent * 100.0) / 100.0},
        {"logical_cores", std::thread::hardware_concurrency()},
        {"load_average", read_load_average()},
        {"status", status},
        {"recommendation", recommendation},
        {"observed_at", now_iso8601()}
    };
    return result;
}

/**
 * @brief 采集磁盘使用情况
 * @param server_id 服务器标识
 * @param path 目标路径
 * @return 磁盘观测结果
 */
json collect_disk_usage(const std::string& server_id, const std::string& path) {
    struct statvfs stat_info {};
    if (statvfs(path.c_str(), &stat_info) != 0) {
        throw std::runtime_error("无法读取磁盘信息: " + path);
    }

    const unsigned long long total_bytes =
        static_cast<unsigned long long>(stat_info.f_blocks) * stat_info.f_frsize;
    const unsigned long long free_bytes =
        static_cast<unsigned long long>(stat_info.f_bavail) * stat_info.f_frsize;
    const unsigned long long used_bytes = total_bytes >= free_bytes ? total_bytes - free_bytes : 0;
    const double used_percent =
        total_bytes == 0 ? 0.0 : static_cast<double>(used_bytes) * 100.0 / static_cast<double>(total_bytes);

    double inode_used_percent = 0.0;
    if (stat_info.f_files > 0) {
        const unsigned long long used_inodes = stat_info.f_files - stat_info.f_ffree;
        inode_used_percent =
            static_cast<double>(used_inodes) * 100.0 / static_cast<double>(stat_info.f_files);
    }

    const std::string status = level_from_threshold(used_percent, 80.0, 90.0);
    std::string recommendation = "磁盘空间正常。";
    if (status == "warning") {
        recommendation = "磁盘使用率偏高，建议清理日志、缓存或迁移大文件。";
    } else if (status == "critical") {
        recommendation = "磁盘空间紧张，建议立即清理无用文件并检查是否存在异常写入。";
    }

    json result = {
        {"tool", "get_disk_usage"},
        {"server_id", server_id},
        {"path", path},
        {"used_percent", std::round(used_percent * 100.0) / 100.0},
        {"free_gb", std::round(bytes_to_gb(free_bytes) * 100.0) / 100.0},
        {"total_gb", std::round(bytes_to_gb(total_bytes) * 100.0) / 100.0},
        {"inode_used_percent", std::round(inode_used_percent * 100.0) / 100.0},
        {"status", status},
        {"recommendation", recommendation},
        {"observed_at", now_iso8601()}
    };
    return result;
}

/**
 * @brief 读取网络接口计数器
 * @param iface 目标网卡名
 * @return 网络计数器
 */
NetworkCounters read_network_counters(const std::string& iface) {
    std::ifstream input("/proc/net/dev");
    if (!input.is_open()) {
        throw std::runtime_error("无法读取 /proc/net/dev");
    }

    std::string line;
    // 跳过表头
    std::getline(input, line);
    std::getline(input, line);

    while (std::getline(input, line)) {
        const auto colon_pos = line.find(':');
        if (colon_pos == std::string::npos) {
            continue;
        }

        const std::string name = trim(line.substr(0, colon_pos));
        if (name != iface) {
            continue;
        }

        std::istringstream stream(line.substr(colon_pos + 1));
        NetworkCounters counters;
        unsigned long long rx_packets = 0;
        unsigned long long rx_fifo = 0;
        unsigned long long rx_frame = 0;
        unsigned long long rx_compressed = 0;
        unsigned long long rx_multicast = 0;
        unsigned long long tx_packets = 0;
        unsigned long long tx_fifo = 0;
        unsigned long long tx_colls = 0;
        unsigned long long tx_carrier = 0;
        unsigned long long tx_compressed = 0;

        stream >> counters.rx_bytes
               >> rx_packets
               >> counters.rx_errors
               >> counters.rx_drops
               >> rx_fifo
               >> rx_frame
               >> rx_compressed
               >> rx_multicast
               >> counters.tx_bytes
               >> tx_packets
               >> counters.tx_errors
               >> counters.tx_drops
               >> tx_fifo
               >> tx_colls
               >> tx_carrier
               >> tx_compressed;
        return counters;
    }

    throw std::runtime_error("未找到网络接口: " + iface);
}

/**
 * @brief 检测默认网络接口
 * @return 网卡名
 */
std::string detect_default_interface() {
    std::ifstream route_input("/proc/net/route");
    if (route_input.is_open()) {
        std::string line;
        std::getline(route_input, line);
        while (std::getline(route_input, line)) {
            std::istringstream stream(line);
            std::string iface;
            std::string destination;
            stream >> iface >> destination;
            if (!iface.empty() && destination == "00000000") {
                return iface;
            }
        }
    }

    std::ifstream dev_input("/proc/net/dev");
    if (dev_input.is_open()) {
        std::string line;
        std::getline(dev_input, line);
        std::getline(dev_input, line);
        while (std::getline(dev_input, line)) {
            const auto colon_pos = line.find(':');
            if (colon_pos == std::string::npos) {
                continue;
            }
            const std::string iface = trim(line.substr(0, colon_pos));
            if (!iface.empty() && iface != "lo") {
                return iface;
            }
        }
    }

    return "lo";
}

/**
 * @brief 采样网络状态
 * @param server_id 服务器标识
 * @param iface 网络接口
 * @param sample_ms 采样窗口
 * @return 网络观测结果
 */
json collect_network_snapshot(const std::string& server_id,
                              const std::string& iface,
                              int sample_ms) {
    const int safe_sample_ms = std::clamp(sample_ms, 200, 5000);
    const std::string effective_iface = iface.empty() ? detect_default_interface() : iface;

    const NetworkCounters first = read_network_counters(effective_iface);
    std::this_thread::sleep_for(std::chrono::milliseconds(safe_sample_ms));
    const NetworkCounters second = read_network_counters(effective_iface);

    const double seconds = static_cast<double>(safe_sample_ms) / 1000.0;
    const double rx_bps =
        seconds <= 0.0 ? 0.0 : static_cast<double>(second.rx_bytes - first.rx_bytes) / seconds;
    const double tx_bps =
        seconds <= 0.0 ? 0.0 : static_cast<double>(second.tx_bytes - first.tx_bytes) / seconds;

    const unsigned long long error_delta =
        (second.rx_errors - first.rx_errors) + (second.tx_errors - first.tx_errors);
    const unsigned long long drop_delta =
        (second.rx_drops - first.rx_drops) + (second.tx_drops - first.tx_drops);

    std::string status = "healthy";
    std::string recommendation = "网络接口状态正常。";
    if (error_delta > 0 || drop_delta > 0) {
        status = "warning";
        recommendation = "检测到丢包或错误包，建议检查链路质量、交换机端口和网卡状态。";
    }

    json result = {
        {"tool", "get_network_snapshot"},
        {"server_id", server_id},
        {"interface", effective_iface},
        {"sample_ms", safe_sample_ms},
        {"rx_bytes_per_sec", std::round(rx_bps * 100.0) / 100.0},
        {"tx_bytes_per_sec", std::round(tx_bps * 100.0) / 100.0},
        {"rx_mbps", std::round(rx_bps * 8.0 / 1000000.0 * 100.0) / 100.0},
        {"tx_mbps", std::round(tx_bps * 8.0 / 1000000.0 * 100.0) / 100.0},
        {"error_packets_delta", error_delta},
        {"drop_packets_delta", drop_delta},
        {"status", status},
        {"recommendation", recommendation},
        {"observed_at", now_iso8601()}
    };
    return result;
}

/**
 * @brief 采集 top 进程
 * @param server_id 服务器标识
 * @param limit 返回数量
 * @param sort_by 排序维度
 * @return top 进程结果
 */
json collect_top_processes(const std::string& server_id,
                           int limit,
                           const std::string& sort_by) {
    const int safe_limit = std::clamp(limit, 1, 20);
    const std::string safe_sort = sort_by == "memory" ? "memory" : "cpu";
    const std::string sort_flag = safe_sort == "memory" ? "--sort=-%mem" : "--sort=-%cpu";
    const std::string command =
        "ps -eo pid=,comm=,%cpu=,%mem= " + sort_flag + " | head -n " + std::to_string(safe_limit);

    std::FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("无法执行 ps 命令");
    }

    json processes = json::array();
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::string line = trim(buffer);
        if (line.empty()) {
            continue;
        }

        std::istringstream stream(line);
        json item;
        stream >> item["pid"] >> item["command"] >> item["cpu_percent"] >> item["memory_percent"];
        if (stream.fail()) {
            continue;
        }
        processes.push_back(item);
    }
    pclose(pipe);

    json result = {
        {"tool", "get_top_processes"},
        {"server_id", server_id},
        {"sort_by", safe_sort},
        {"limit", safe_limit},
        {"processes", processes},
        {"observed_at", now_iso8601()}
    };
    return result;
}

/**
 * @brief 采集整体系统概览
 * @param server_id 服务器标识
 * @return 系统概览结果
 */
json collect_system_overview(const std::string& server_id) {
    const json cpu = collect_cpu_snapshot(server_id, 800);
    const json disk = collect_disk_usage(server_id, "/");
    const json network = collect_network_snapshot(server_id, "", 800);
    const json top_processes = collect_top_processes(server_id, 5, "cpu");

    std::vector<std::string> issues;
    std::vector<std::string> recommendations;
    std::string overall_status = "healthy";

    // 根据 CPU、磁盘、网络结果生成运维侧的快速摘要。
    if (cpu.value("status", "healthy") == "critical") {
        issues.push_back("CPU 使用率很高");
        recommendations.push_back(cpu.value("recommendation", ""));
        overall_status = "critical";
    } else if (cpu.value("status", "healthy") == "warning") {
        issues.push_back("CPU 使用率偏高");
        recommendations.push_back(cpu.value("recommendation", ""));
        if (overall_status != "critical") {
            overall_status = "warning";
        }
    }

    if (disk.value("status", "healthy") == "critical") {
        issues.push_back("磁盘空间紧张");
        recommendations.push_back(disk.value("recommendation", ""));
        overall_status = "critical";
    } else if (disk.value("status", "healthy") == "warning") {
        issues.push_back("磁盘使用率偏高");
        recommendations.push_back(disk.value("recommendation", ""));
        if (overall_status != "critical") {
            overall_status = "warning";
        }
    }

    if (network.value("status", "healthy") == "warning") {
        issues.push_back("网络接口存在丢包或错误包");
        recommendations.push_back(network.value("recommendation", ""));
        if (overall_status != "critical") {
            overall_status = "warning";
        }
    }

    if (issues.empty()) {
        issues.push_back("当前未发现明显异常");
        recommendations.push_back("保持定期巡检，持续关注 CPU、磁盘和网络趋势。");
    }

    json result = {
        {"tool", "get_system_overview"},
        {"server_id", server_id},
        {"overall_status", overall_status},
        {"cpu", cpu},
        {"disk", disk},
        {"network", network},
        {"top_processes", top_processes["processes"]},
        {"issues", issues},
        {"recommendations", recommendations},
        {"observed_at", now_iso8601()}
    };
    return result;
}

/**
 * @brief 检查指定端口是否存在监听或绑定
 * @param server_id 服务器标识
 * @param port 目标端口
 * @param protocol 协议过滤
 * @return 端口监听结果
 */
json collect_port_listening(const std::string& server_id,
                            int port,
                            const std::string& protocol) {
    if (port <= 0 || port > 65535) {
        throw std::runtime_error("port 必须在 1 到 65535 之间");
    }

    const std::string safe_protocol = normalize_protocol_filter(protocol);
    const std::string command =
        "ss -H -ltnup 'sport = :" + std::to_string(port) + "' 2>/dev/null";
    const CommandResult command_result = run_command_capture(command);

    if (command_result.exit_code == 127) {
        throw std::runtime_error("未找到 ss 命令，无法检查端口监听状态");
    }

    json entries = json::array();
    std::istringstream stream(command_result.output);
    std::string line;
    while (std::getline(stream, line)) {
        const std::string trimmed_line = trim(line);
        if (trimmed_line.empty()) {
            continue;
        }

        std::istringstream line_stream(trimmed_line);
        std::string socket_protocol;
        std::string state;
        std::string recv_q;
        std::string send_q;
        std::string local_endpoint;
        std::string peer_endpoint;
        line_stream >> socket_protocol >> state >> recv_q >> send_q >> local_endpoint >> peer_endpoint;
        if (line_stream.fail()) {
            continue;
        }

        if (safe_protocol != "all" && socket_protocol != safe_protocol) {
            continue;
        }

        std::string owners_text;
        std::getline(line_stream, owners_text);
        const auto [local_address, local_port] = split_endpoint(local_endpoint);
        const auto [peer_address, peer_port] = split_endpoint(peer_endpoint);

        entries.push_back({
            {"protocol", socket_protocol},
            {"state", state},
            {"recv_q", recv_q},
            {"send_q", send_q},
            {"local_address", local_address},
            {"local_port", local_port},
            {"peer_address", peer_address},
            {"peer_port", peer_port},
            {"owners", parse_socket_owners(owners_text)}
        });
    }

    const bool is_listening = !entries.empty();
    std::string recommendation = "端口当前未检测到监听或绑定记录。";
    if (is_listening) {
        recommendation = "端口已被本机进程占用，可根据 owners 字段继续定位具体服务。";
    }

    json result = {
        {"tool", "check_port_listening"},
        {"server_id", server_id},
        {"port", port},
        {"protocol_filter", safe_protocol},
        {"is_listening", is_listening},
        {"listener_count", entries.size()},
        {"entries", entries},
        {"recommendation", recommendation},
        {"observed_at", now_iso8601()}
    };
    return result;
}

/**
 * @brief 对目标主机执行 ping 连通性诊断
 * @param server_id 服务器标识
 * @param host 目标主机名或 IP
 * @param count 探测次数
 * @param timeout_sec 单包超时秒数
 * @param ip_version IP 版本偏好
 * @return ping 诊断结果
 */
json collect_ping_host(const std::string& server_id,
                       const std::string& host,
                       int count,
                       int timeout_sec,
                       const std::string& ip_version) {
    const std::string safe_host = sanitize_host_or_throw(host);
    const int safe_count = std::clamp(count, 1, 10);
    const int safe_timeout_sec = std::clamp(timeout_sec, 1, 10);

    std::string safe_ip_version = trim(ip_version);
    std::transform(safe_ip_version.begin(), safe_ip_version.end(), safe_ip_version.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    if (safe_ip_version.empty()) {
        safe_ip_version = "auto";
    }
    if (safe_ip_version != "auto" &&
        safe_ip_version != "4" &&
        safe_ip_version != "6") {
        throw std::runtime_error("ip_version 只支持 auto、4 或 6");
    }

    std::string command = "ping -n -c " + std::to_string(safe_count) +
        " -W " + std::to_string(safe_timeout_sec);
    if (safe_ip_version == "4") {
        command += " -4";
    } else if (safe_ip_version == "6") {
        command += " -6";
    }
    command += " " + safe_host + " 2>&1";

    const CommandResult command_result = run_command_capture(command);
    if (command_result.exit_code == 127) {
        throw std::runtime_error("未找到 ping 命令，无法执行连通性诊断");
    }

    std::string resolved_ip;
    int transmitted = 0;
    int received = 0;
    double packet_loss_percent = 100.0;
    double min_rtt_ms = 0.0;
    double avg_rtt_ms = 0.0;
    double max_rtt_ms = 0.0;
    double mdev_rtt_ms = 0.0;
    bool has_rtt = false;

    std::istringstream output_stream(command_result.output);
    std::string line;
    const std::regex ping_header_regex(R"(^PING\s+\S+\s+\(([^)]+)\))");
    const std::regex packet_regex(
        R"(([0-9]+)\s+packets transmitted,\s+([0-9]+)(?:\s+packets)? received,\s+([0-9.]+)% packet loss)");
    const std::regex rtt_regex(
        R"((?:rtt|round-trip) min/avg/max/(?:mdev|stddev) = ([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+) ms)");

    while (std::getline(output_stream, line)) {
        std::smatch match;
        if (resolved_ip.empty() &&
            std::regex_search(line, match, ping_header_regex)) {
            resolved_ip = match[1].str();
        }
        if (std::regex_search(line, match, packet_regex)) {
            transmitted = std::stoi(match[1].str());
            received = std::stoi(match[2].str());
            packet_loss_percent = std::stod(match[3].str());
        }
        if (std::regex_search(line, match, rtt_regex)) {
            min_rtt_ms = std::stod(match[1].str());
            avg_rtt_ms = std::stod(match[2].str());
            max_rtt_ms = std::stod(match[3].str());
            mdev_rtt_ms = std::stod(match[4].str());
            has_rtt = true;
        }
    }

    std::string status = "reachable";
    std::string recommendation = "链路连通性正常。";
    if (received == 0) {
        status = "unreachable";
        recommendation = "未收到任何响应，建议检查目标主机状态、DNS 解析、防火墙和路由。";
    } else if (packet_loss_percent > 0.0) {
        status = "degraded";
        recommendation = "检测到丢包，建议继续检查链路质量、交换机端口和网络拥塞情况。";
    }

    json result = {
        {"tool", "ping_host"},
        {"server_id", server_id},
        {"host", safe_host},
        {"resolved_ip", resolved_ip},
        {"count", safe_count},
        {"timeout_sec", safe_timeout_sec},
        {"ip_version", safe_ip_version},
        {"status", status},
        {"transmitted", transmitted},
        {"received", received},
        {"packet_loss_percent", packet_loss_percent},
        {"has_rtt", has_rtt},
        {"min_rtt_ms", min_rtt_ms},
        {"avg_rtt_ms", avg_rtt_ms},
        {"max_rtt_ms", max_rtt_ms},
        {"mdev_rtt_ms", mdev_rtt_ms},
        {"command_exit_code", command_result.exit_code},
        {"recommendation", recommendation},
        {"raw_output", command_result.output},
        {"observed_at", now_iso8601()}
    };
    return result;
}

/**
 * @brief 统一处理工具请求
 * @param tool_name 工具名
 * @param args 工具参数
 * @return 工具结果 JSON
 */
json handle_tool_request(const std::string& tool_name, const json& args) {
    const std::string server_id = get_string_arg(args, "server_id", "local");

    if (tool_name == "get_system_overview") {
        return collect_system_overview(server_id);
    }
    if (tool_name == "get_cpu_snapshot") {
        return collect_cpu_snapshot(server_id, get_int_arg(args, "sample_ms", 1000));
    }
    if (tool_name == "get_disk_usage") {
        return collect_disk_usage(server_id, get_string_arg(args, "path", "/"));
    }
    if (tool_name == "get_network_snapshot") {
        return collect_network_snapshot(
            server_id,
            get_string_arg(args, "interface", ""),
            get_int_arg(args, "sample_ms", 1000));
    }
    if (tool_name == "get_top_processes") {
        return collect_top_processes(
            server_id,
            get_int_arg(args, "limit", 5),
            get_string_arg(args, "sort_by", "cpu"));
    }
    if (tool_name == "check_port_listening") {
        return collect_port_listening(
            server_id,
            get_int_arg(args, "port", 0),
            get_string_arg(args, "protocol", "all"));
    }
    if (tool_name == "ping_host") {
        return collect_ping_host(
            server_id,
            get_string_arg(args, "host", ""),
            get_int_arg(args, "count", 4),
            get_int_arg(args, "timeout_sec", 2),
            get_string_arg(args, "ip_version", "auto"));
    }

    throw std::runtime_error("未知工具: " + tool_name);
}

static PluginTool methods[] = {
    {
        "get_system_overview",
        "Collects an overall local server overview including CPU, disk, network and top processes.",
        R"({
            "$schema": "http://json-schema.org/draft-07/schema#",
            "type": "object",
            "properties": {
                "server_id": { "type": "string", "description": "Logical server identifier used for labeling. Defaults to local." }
            },
            "additionalProperties": false
        })"
    },
    {
        "get_cpu_snapshot",
        "Samples local CPU utilization and load average for a short interval.",
        R"({
            "$schema": "http://json-schema.org/draft-07/schema#",
            "type": "object",
            "properties": {
                "server_id": { "type": "string", "description": "Logical server identifier used for labeling. Defaults to local." },
                "sample_ms": { "type": "integer", "minimum": 200, "maximum": 5000, "description": "Sampling window in milliseconds." }
            },
            "additionalProperties": false
        })"
    },
    {
        "get_disk_usage",
        "Collects local disk usage information for the specified path.",
        R"({
            "$schema": "http://json-schema.org/draft-07/schema#",
            "type": "object",
            "properties": {
                "server_id": { "type": "string", "description": "Logical server identifier used for labeling. Defaults to local." },
                "path": { "type": "string", "description": "Filesystem path to inspect. Defaults to /." }
            },
            "additionalProperties": false
        })"
    },
    {
        "get_network_snapshot",
        "Samples local network throughput, drops and errors on a network interface.",
        R"({
            "$schema": "http://json-schema.org/draft-07/schema#",
            "type": "object",
            "properties": {
                "server_id": { "type": "string", "description": "Logical server identifier used for labeling. Defaults to local." },
                "interface": { "type": "string", "description": "Network interface name. Defaults to the default route interface." },
                "sample_ms": { "type": "integer", "minimum": 200, "maximum": 5000, "description": "Sampling window in milliseconds." }
            },
            "additionalProperties": false
        })"
    },
    {
        "get_top_processes",
        "Returns top local processes sorted by CPU or memory.",
        R"({
            "$schema": "http://json-schema.org/draft-07/schema#",
            "type": "object",
            "properties": {
                "server_id": { "type": "string", "description": "Logical server identifier used for labeling. Defaults to local." },
                "limit": { "type": "integer", "minimum": 1, "maximum": 20, "description": "Maximum number of processes to return." },
                "sort_by": { "type": "string", "enum": ["cpu", "memory"], "description": "Sort dimension." }
            },
            "additionalProperties": false
        })"
    },
    {
        "check_port_listening",
        "Checks whether a local TCP or UDP port is listening and which process owns it.",
        R"({
            "$schema": "http://json-schema.org/draft-07/schema#",
            "type": "object",
            "properties": {
                "server_id": { "type": "string", "description": "Logical server identifier used for labeling. Defaults to local." },
                "port": { "type": "integer", "minimum": 1, "maximum": 65535, "description": "Port number to inspect." },
                "protocol": { "type": "string", "enum": ["all", "tcp", "udp"], "description": "Protocol filter. Defaults to all." }
            },
            "required": ["port"],
            "additionalProperties": false
        })"
    },
    {
        "ping_host",
        "Runs a local ping diagnostic to a host and returns packet loss plus latency summary.",
        R"({
            "$schema": "http://json-schema.org/draft-07/schema#",
            "type": "object",
            "properties": {
                "server_id": { "type": "string", "description": "Logical server identifier used for labeling. Defaults to local." },
                "host": { "type": "string", "description": "Hostname or IP address to probe." },
                "count": { "type": "integer", "minimum": 1, "maximum": 10, "description": "Number of ICMP echo requests to send." },
                "timeout_sec": { "type": "integer", "minimum": 1, "maximum": 10, "description": "Per-request timeout in seconds." },
                "ip_version": { "type": "string", "enum": ["auto", "4", "6"], "description": "IP version preference. Defaults to auto." }
            },
            "required": ["host"],
            "additionalProperties": false
        })"
    }
};

}  // namespace

const char* GetNameImpl() { return "server-observer-tools"; }
const char* GetVersionImpl() { return "1.0.0"; }
PluginType GetTypeImpl() { return PLUGIN_TYPE_TOOLS; }

/**
 * @brief 初始化插件
 * @return 1 表示成功
 */
int InitializeImpl() {
    return 1;
}

/**
 * @brief 处理 MCP 工具请求
 * @param req MCP 请求 JSON
 * @return 响应字符串
 */
char* HandleRequestImpl(const char* req) {
    std::string response;

    try {
        const json request = json::parse(req);
        const std::string tool_name = request["params"]["name"].get<std::string>();
        const json args = request["params"].contains("arguments")
            ? request["params"]["arguments"]
            : json::object();

        // 将本地观测结果封装为结构化 JSON 文本，方便上层 Agent 继续推理。
        const json payload = handle_tool_request(tool_name, args);
        response = make_text_response(payload.dump(2), false);
    } catch (const std::exception& e) {
        response = make_text_response(std::string("Error: ") + e.what(), true);
    }

    char* buffer = new char[response.length() + 1];
#ifdef _WIN32
    strcpy_s(buffer, response.length() + 1, response.c_str());
#else
    std::strcpy(buffer, response.c_str());
#endif
    return buffer;
}

/**
 * @brief 关闭插件
 */
void ShutdownImpl() {
}

/**
 * @brief 获取工具数量
 * @return 工具数量
 */
int GetToolCountImpl() {
    return sizeof(methods) / sizeof(methods[0]);
}

/**
 * @brief 获取指定索引的工具定义
 * @param index 工具索引
 * @return 工具指针
 */
const PluginTool* GetToolImpl(int index) {
    if (index < 0 || index >= GetToolCountImpl()) {
        return nullptr;
    }
    return &methods[index];
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
    // 插件使用静态实例，无需额外清理。
}
