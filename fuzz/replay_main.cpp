// Replays saved fuzz inputs without libFuzzer, so every build (any compiler, any
// sanitizer) checks them. The arguments are files or directories, and each file is one
// input. A disagreement aborts inside run_one, which fails the test. Files ending in `.md`
// are notes and are skipped.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <vector>

#include "book_fuzzer.hpp"

namespace {

void replay_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
    litbook::fuzz::run_one(bytes.data(), bytes.size());
}

}  // namespace

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    int files = 0;
    for (const char* arg : std::span(argv, static_cast<std::size_t>(argc)).subspan(1)) {
        const fs::path path(arg);
        if (fs::is_directory(path)) {
            for (const auto& entry : fs::directory_iterator(path)) {
                if (entry.is_regular_file() && entry.path().extension() != ".md") {
                    replay_file(entry.path());
                    ++files;
                }
            }
        } else if (fs::is_regular_file(path)) {
            replay_file(path);
            ++files;
        } else {
            std::fprintf(stderr, "no such file or directory: %s\n", arg);  // NOLINT
            return 1;
        }
    }
    std::printf("replayed %d inputs\n", files);  // NOLINT
    return files > 0 ? 0 : 1;                    // an empty corpus means the test checked nothing
}
