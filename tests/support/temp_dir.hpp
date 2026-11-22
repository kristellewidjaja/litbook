#pragma once

#include <stdlib.h>  // mkdtemp

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace litbook::testing {

// A fresh directory under the system's temporary directory (/tmp in the container),
// removed with everything in it when the object goes away. Tests that write journal
// files put them here, so runs side by side never share a file and nothing is left
// behind.
class TempDir {
public:
    TempDir() {
        std::string pattern = (std::filesystem::temp_directory_path() / "litbook-XXXXXX").string();
        if (::mkdtemp(pattern.data()) == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = pattern;
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    // The path of a file called `name` inside the directory.
    [[nodiscard]] std::string file(std::string_view name) const {
        return (std::filesystem::path(path_) / name).string();
    }

private:
    std::string path_;
};

}  // namespace litbook::testing
