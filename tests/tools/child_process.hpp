#pragma once

#include <gtest/gtest.h>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern char** environ;  // NOLINT(readability-redundant-declaration): posix_spawn needs it.

// Helpers for the end-to-end tests that run the litbook programs.

namespace litbook::testing {

// A program running as a child process, with its standard output in a pipe. The
// destructor kills it if it is still running, so a failed test never leaves it behind.
class ChildProcess {
public:
    ChildProcess(const char* program, std::vector<std::string> args) {
        args.insert(args.begin(), program);
        std::array<int, 2> pipe_fds{};
        if (::pipe(pipe_fds.data()) != 0) {
            throw std::runtime_error("pipe failed");
        }
        posix_spawn_file_actions_t actions{};
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (std::string& a : args) {
            argv.push_back(a.data());
        }
        argv.push_back(nullptr);
        const int rc = posix_spawn(&pid_, args[0].c_str(), &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(pipe_fds[1]);
        if (rc != 0) {
            ::close(pipe_fds[0]);
            throw std::runtime_error("posix_spawn failed");
        }
        out_.reset(::fdopen(pipe_fds[0], "r"));
    }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&&) = delete;
    ChildProcess& operator=(ChildProcess&&) = delete;

    ~ChildProcess() {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            ::waitpid(pid_, nullptr, 0);
        }
    }

    // The next line the program printed, without its newline, or "" at the end.
    std::string line() {
        std::array<char, 512> buffer{};
        if (std::fgets(buffer.data(), static_cast<int>(buffer.size()), out_.get()) == nullptr) {
            return "";
        }
        std::string s(buffer.data());
        if (!s.empty() && s.back() == '\n') {
            s.pop_back();
        }
        return s;
    }

    // Sends a signal and returns the exit status, or -1 if it did not exit normally.
    int stop(int signal) {
        ::kill(pid_, signal);
        int status = 0;
        ::waitpid(pid_, &status, 0);
        pid_ = -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    // Waits for a program that exits by itself. Returns its exit status, -1 if it did not
    // exit normally, or -2 if it is still running after `limit` (it is killed later).
    int wait_for_exit(std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            int status = 0;
            if (::waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return -2;
    }

private:
    pid_t pid_ = -1;
    // The read end of the pipe as a FILE, closed by fclose when the object goes away.
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> out_{nullptr, &std::fclose};
};

// Reads the gateway's "listening on 127.0.0.1:PORT" line and returns the port.
inline std::uint16_t gateway_port(ChildProcess& gateway) {
    const std::string first = gateway.line();
    const auto colon = first.rfind(':');
    EXPECT_NE(colon, std::string::npos) << first;
    EXPECT_NE(first.find("listening on 127.0.0.1:"), std::string::npos) << first;
    return static_cast<std::uint16_t>(std::stoi(first.substr(colon + 1)));
}

}  // namespace litbook::testing
