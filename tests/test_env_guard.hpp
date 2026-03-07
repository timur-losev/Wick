// cpp/tests/test_env_guard.hpp
// RAII guard for environment variables in tests. Restores on destruction.
#pragma once

#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>

namespace waxcpp::tests {

inline std::optional<std::string> GetEnvVar(const char* key) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&value, &len, key) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string out(value);
    std::free(value);
    if (out.empty()) {
        return std::nullopt;
    }
    return out;
#else
    const char* value = std::getenv(key);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

inline void SetEnvVar(const char* key, const std::string& value) {
#if defined(_MSC_VER)
    if (_putenv_s(key, value.c_str()) != 0) {
        throw std::runtime_error(std::string("failed to set env var: ") + key);
    }
#else
    if (setenv(key, value.c_str(), 1) != 0) {
        throw std::runtime_error(std::string("failed to set env var: ") + key);
    }
#endif
}

inline void UnsetEnvVar(const char* key) {
#if defined(_MSC_VER)
    if (_putenv_s(key, "") != 0) {
        throw std::runtime_error(std::string("failed to unset env var: ") + key);
    }
#else
    if (unsetenv(key) != 0) {
        throw std::runtime_error(std::string("failed to unset env var: ") + key);
    }
#endif
}

class EnvVarGuard {
 public:
    explicit EnvVarGuard(const char* key) : key_(key), previous_(GetEnvVar(key)) {}
    ~EnvVarGuard() noexcept {
        try {
            if (previous_.has_value()) {
                SetEnvVar(key_.c_str(), *previous_);
            } else {
                UnsetEnvVar(key_.c_str());
            }
        } catch (...) {
        }
    }
    EnvVarGuard(const EnvVarGuard&) = delete;
    EnvVarGuard& operator=(const EnvVarGuard&) = delete;

 private:
    std::string key_{};
    std::optional<std::string> previous_{};
};

}  // namespace waxcpp::tests
