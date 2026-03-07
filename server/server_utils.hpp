// cpp/server/server_utils.hpp
// Shared server-side utilities: env access, JSON escaping, file I/O, string helpers.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace waxcpp::server {

// Platform-safe environment variable access.  Returns nullopt for unset or empty values.
[[nodiscard]] std::optional<std::string> EnvString(const char* name);

// RFC 8259 §7 compliant JSON string value escaping (no surrounding quotes).
// Escapes: \\ \" \n \r \t \b \f and U+0000..U+001F control characters.
[[nodiscard]] std::string JsonEscape(const std::string& value);

// ASCII-only case-fold to lowercase (returns a copy).
[[nodiscard]] std::string ToAsciiLower(std::string_view value);

// Read entire file into a string (binary mode).
[[nodiscard]] std::string ReadFileText(const std::filesystem::path& path);

// Write entire string to a file (binary mode, truncates existing).
void WriteFileText(const std::filesystem::path& path, std::string_view content);

}  // namespace waxcpp::server
