#include "ai_orchestrator/session_manager.hpp"

#include <array>
#include <random>
#include <regex>
#include <stdexcept>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace ai_orchestrator::dag {

namespace {

bool is_symlink_path(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path, error));
}

}  // namespace

SessionManager::SessionManager(std::filesystem::path root)
    : root_(std::filesystem::absolute(std::move(root))) {
    std::error_code error;
    std::filesystem::create_directories(root_, error);
    if (error) throw std::runtime_error("cannot create sessions root: " + error.message());
}

bool SessionManager::valid_id(const std::string& value) {
    static const std::regex pattern("^[a-f0-9]{32}$");
    return std::regex_match(value, pattern);
}

bool SessionManager::ensure_directories(const std::string& session_id) {
    if (!valid_id(session_id)) return false;
    const auto directory = root_ / session_id;
    if (is_symlink_path(directory) || is_symlink_path(directory / "workspace") ||
        is_symlink_path(directory / "artifacts") || is_symlink_path(directory / "audit")) {
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(directory / "workspace", error);
    if (error) return false;
    std::filesystem::create_directories(directory / "artifacts", error);
    if (error) return false;
    std::filesystem::create_directories(directory / "audit", error);
    if (error) return false;
    if (is_symlink_path(directory) || is_symlink_path(directory / "workspace") ||
        is_symlink_path(directory / "artifacts") || is_symlink_path(directory / "audit")) {
        return false;
    }
#ifndef _WIN32
    ::chmod(directory.c_str(), 0700);
    ::chmod((directory / "workspace").c_str(), 0700);
    ::chmod((directory / "artifacts").c_str(), 0700);
    ::chmod((directory / "audit").c_str(), 0700);
#endif
    return true;
}

std::string SessionManager::create() {
    static constexpr char hex[] = "0123456789abcdef";
    std::random_device random;
    std::lock_guard<std::mutex> lock(mutex_);
    for (int attempt = 0; attempt < 32; ++attempt) {
        std::string id(32, '0');
        for (auto& ch : id) ch = hex[random() & 0x0f];
        if (std::filesystem::exists(root_ / id)) continue;
        if (!ensure_directories(id)) throw std::runtime_error("cannot create session directories");
        active_.insert(id);
        return id;
    }
    throw std::runtime_error("cannot allocate unique session id");
}

bool SessionManager::resume(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid_id(session_id) || is_symlink_path(root_ / session_id) ||
        is_symlink_path(root_ / session_id / "workspace") ||
        !std::filesystem::is_directory(root_ / session_id / "workspace")) return false;
    if (!ensure_directories(session_id)) return false;
    active_.insert(session_id);
    return true;
}

bool SessionManager::exists(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_.count(session_id) > 0 &&
           std::filesystem::is_directory(root_ / session_id / "workspace");
}

}  // namespace ai_orchestrator::dag
