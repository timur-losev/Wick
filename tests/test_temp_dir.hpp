// cpp/tests/test_temp_dir.hpp
// RAII wrappers for temporary test directories and store paths.
#pragma once

#include "temp_artifacts.hpp"

#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>

namespace waxcpp::tests {

class ScopedTempDir {
 public:
    explicit ScopedTempDir(const std::string& prefix) {
        const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        std::ostringstream name;
        name << prefix << static_cast<long long>(now);
        path_ = std::filesystem::temp_directory_path() / name.str();
        std::error_code ec;
        std::filesystem::create_directories(path_, ec);
    }

    ~ScopedTempDir() noexcept {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
    std::filesystem::path path_{};
};

class ScopedStorePath {
 public:
    explicit ScopedStorePath(const std::string& prefix) {
        const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        std::ostringstream name;
        name << prefix << static_cast<long long>(now) << ".mv2s";
        path_ = std::filesystem::temp_directory_path() / name.str();
    }

    ~ScopedStorePath() noexcept {
        CleanupStoreArtifacts(path_);
        std::error_code ec;
        const auto cp = checkpoint_path();
        std::filesystem::remove(cp, ec);
        ec.clear();
        std::filesystem::remove(cp.string() + ".scan_manifest", ec);
        ec.clear();
        std::filesystem::remove(cp.string() + ".chunk_manifest", ec);
        ec.clear();
        std::filesystem::remove(cp.string() + ".file_manifest", ec);
    }

    ScopedStorePath(const ScopedStorePath&) = delete;
    ScopedStorePath& operator=(const ScopedStorePath&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    [[nodiscard]] std::filesystem::path checkpoint_path() const {
        return std::filesystem::path(path_.string() + ".index.checkpoint");
    }

    [[nodiscard]] std::filesystem::path scan_manifest_path() const {
        return std::filesystem::path(checkpoint_path().string() + ".scan_manifest");
    }

    [[nodiscard]] std::filesystem::path chunk_manifest_path() const {
        return std::filesystem::path(checkpoint_path().string() + ".chunk_manifest");
    }

    [[nodiscard]] std::filesystem::path file_manifest_path() const {
        return std::filesystem::path(checkpoint_path().string() + ".file_manifest");
    }

 private:
    std::filesystem::path path_{};
};

}  // namespace waxcpp::tests
