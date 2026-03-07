#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace waxcpp::tests {

inline bool IsTruthy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value == "1" || value == "true" || value == "yes" || value == "on";
}

inline bool LoggingEnabled() {
  static const bool enabled = []() {
#if defined(NDEBUG)
    constexpr bool kDefault = false;
#else
    constexpr bool kDefault = true;
#endif
#if defined(_MSC_VER)
    char* env = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&env, &len, "WAXCPP_TEST_LOG") != 0 || env == nullptr) {
      return kDefault;
    }
    std::string value(env);
    std::free(env);
    return IsTruthy(value);
#else
    const char* env = std::getenv("WAXCPP_TEST_LOG");
    if (env == nullptr) {
      return kDefault;
    }
    return IsTruthy(env);
#endif
  }();
  return enabled;
}

inline void Log(std::string_view message) {
  if (!LoggingEnabled()) {
    return;
  }
  std::cout << "[waxcpp-test] " << message << "\n";
}

inline void LogError(std::string_view message) {
  if (!LoggingEnabled()) {
    return;
  }
  std::cerr << "[waxcpp-test] ERROR: " << message << "\n";
}

inline void LogKV(std::string_view key, std::string_view value) {
  if (!LoggingEnabled()) {
    return;
  }
  std::cout << "[waxcpp-test] " << key << "=" << value << "\n";
}

inline void LogKV(std::string_view key, const std::string& value) {
  LogKV(key, std::string_view(value));
}

inline void LogKV(std::string_view key, const char* value) {
  LogKV(key, std::string_view(value != nullptr ? value : "(null)"));
}

template <std::size_t N>
inline void LogKV(std::string_view key, const char (&value)[N]) {
  static_assert(N > 0, "char array log value must include null terminator");
  LogKV(key, std::string_view(value, N - 1));
}

inline void LogKV(std::string_view key, std::uint64_t value) {
  LogKV(key, std::to_string(value));
}

inline void LogKV(std::string_view key, bool value) {
  LogKV(key, std::string_view(value ? "true" : "false"));
}

}  // namespace waxcpp::tests
