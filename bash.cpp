// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "bash.h"

#include "bash-safety.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

    void closeDescriptor(int &descriptor) {
        if (descriptor != -1) {
            close(descriptor);
            descriptor = -1;
        }
    }

    void signalProcess(const pid_t pid, const int signal_number) {
        // The child creates its own process group before exec. Signalling the
        // group also reaches commands that spawn descendants. Fall back to the
        // child itself if group creation lost a race or was unavailable.
        if (getpgid(pid) == pid) {
            if (kill(-pid, signal_number) == 0 || errno == ESRCH) {
                return;
            }
        }
        kill(pid, signal_number);
    }

    void killAndReap(const pid_t pid) {
        signalProcess(pid, SIGKILL);
        int status = 0;
        while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
        }
    }

    microcodex::BashCommandResult runProcess(
        const std::vector<std::string> &arguments,
        const std::stop_token stop_token,
        const std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt) {
        if (stop_token.stop_requested()) {
            throw std::runtime_error("Command interrupted");
        }
        if (arguments.empty()) {
            throw std::invalid_argument("command must not be empty");
        }

        std::vector<char *> argv;
        argv.reserve(arguments.size() + 1);
        for (const std::string &argument : arguments) {
            argv.push_back(const_cast<char *>(argument.c_str()));
        }
        // execvp() requires a null-terminated argument vector.
        argv.push_back(nullptr);

        int stdout_pipe[2]{-1, -1};
        int stderr_pipe[2]{-1, -1};
        if (pipe(stdout_pipe) == -1) {
            throw std::runtime_error(std::string("pipe failed: ") + std::strerror(errno));
        }
        if (pipe(stderr_pipe) == -1) {
            closeDescriptor(stdout_pipe[0]);
            closeDescriptor(stdout_pipe[1]);
            throw std::runtime_error(std::string("pipe failed: ") + std::strerror(errno));
        }

        const pid_t pid = fork();
        if (pid == -1) {
            closeDescriptor(stdout_pipe[0]);
            closeDescriptor(stdout_pipe[1]);
            closeDescriptor(stderr_pipe[0]);
            closeDescriptor(stderr_pipe[1]);
            throw std::runtime_error(std::string("fork failed: ") + std::strerror(errno));
        }

        if (pid == 0) {
            closeDescriptor(stdout_pipe[0]);
            closeDescriptor(stderr_pipe[0]);

            // A separate group lets interruption terminate the whole command
            // tree instead of leaving grandchildren running in the background.
            setpgid(0, 0);
            if (dup2(stdout_pipe[1], STDOUT_FILENO) == -1 || dup2(stderr_pipe[1], STDERR_FILENO) == -1) {
                _exit(126);
            }
            closeDescriptor(stdout_pipe[1]);
            closeDescriptor(stderr_pipe[1]);

            execvp(argv[0], argv.data());

            // execvp() returns only on failure. Use the C system call here
            // because C++ streams are not safe after fork in a multithreaded process.
            const char *message = std::strerror(errno);
            ::write(STDERR_FILENO, message, std::strlen(message));
            ::write(STDERR_FILENO, "\n", 1);
            _exit(127);
        }

        closeDescriptor(stdout_pipe[1]);
        closeDescriptor(stderr_pipe[1]);
        // The child also calls setpgid(); doing it in the parent closes the
        // short race before the child reaches that line. EACCES/ESRCH are safe
        // here because the child has either exec'd or already exited.
        setpgid(pid, pid);

        microcodex::BashCommandResult result{};
        std::array<char, 4096> buffer{};
        pollfd descriptors[2]{{stdout_pipe[0], POLLIN, 0}, {stderr_pipe[0], POLLIN, 0}};
        int open_streams = 2;
        int status = 0;
        bool child_exited = false;
        bool sent_interrupt = false;
        bool sent_kill = false;
        bool timed_out = false;
        std::chrono::steady_clock::time_point interrupt_time;

        while (open_streams > 0 || !child_exited) {
            if (deadline && std::chrono::steady_clock::now() >= *deadline &&
                !sent_kill && !child_exited) {
                signalProcess(pid, SIGKILL);
                sent_kill = true;
                timed_out = true;
            }
            if (stop_token.stop_requested() && !sent_interrupt && !child_exited) {
                signalProcess(pid, SIGINT);
                sent_interrupt = true;
                interrupt_time = std::chrono::steady_clock::now();
            }
            if (sent_interrupt && !sent_kill && !child_exited && std::chrono::steady_clock::now() - interrupt_time >= std::chrono::seconds(1)) {
                // SIGINT gives well-behaved commands time to clean up. SIGKILL
                // guarantees that interrupt() cannot wait forever otherwise.
                signalProcess(pid, SIGKILL);
                sent_kill = true;
            }

            if (!child_exited) {
                const pid_t wait_result = waitpid(pid, &status, WNOHANG);
                if (wait_result == pid) {
                    child_exited = true;
                } else if (wait_result == -1 && errno != EINTR) {
                    closeDescriptor(descriptors[0].fd);
                    closeDescriptor(descriptors[1].fd);
                    throw std::runtime_error(std::string("waitpid failed: ") + std::strerror(errno));
                }
            }

            if (open_streams == 0) {
                // poll() with no live descriptors provides a short cancellable
                // wait while the process finishes after closing its streams.
                poll(nullptr, 0, 50);
                continue;
            }

            const int poll_result = poll(descriptors, 2, 100);
            if (poll_result == -1) {
                if (errno == EINTR) {
                    continue;
                }
                closeDescriptor(descriptors[0].fd);
                closeDescriptor(descriptors[1].fd);
                killAndReap(pid);
                throw std::runtime_error(std::string("poll failed: ") + std::strerror(errno));
            }

            for (std::size_t index = 0; index < 2; ++index) {
                pollfd &descriptor = descriptors[index];
                if (descriptor.fd == -1) {
                    continue;
                }

                if ((descriptor.revents & (POLLIN | POLLHUP)) != 0) {
                    const ssize_t count = ::read(descriptor.fd, buffer.data(), buffer.size());
                    if (count > 0) {
                        std::string &destination = index == 0 ? result.stdout : result.stderr;
                        destination.append(buffer.data(), static_cast<std::size_t>(count));
                    } else if (count == 0) {
                        closeDescriptor(descriptor.fd);
                        --open_streams;
                    } else if (errno != EINTR) {
                        closeDescriptor(descriptor.fd);
                        --open_streams;
                    }
                }

                if (descriptor.fd != -1 && (descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
                    closeDescriptor(descriptor.fd);
                    --open_streams;
                }
            }
        }

        if (timed_out) {
            throw std::runtime_error("Command timed out");
        }

        if (WIFEXITED(status)) {
            result.error_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            result.error_code = 128 + WTERMSIG(status);
        } else {
            result.error_code = -1;
        }
        return result;
    }

    std::string shellSingleQuote(const std::string_view value) {
        std::string quoted;
        quoted.reserve(value.size() + 8);
        for (const char character : value) {
            if (character == '\'') {
                quoted += "'\\''";
            } else {
                quoted += character;
            }
        }
        return quoted;
    }

    std::string shellRcPrefix(const std::filesystem::path &shell) {
        const std::string name = shell.filename().string();
        if (name == "zsh") {
            return R"(if [[ -n "$ZDOTDIR" ]]; then __microcodex_rc="$ZDOTDIR/.zshrc"; else __microcodex_rc="$HOME/.zshrc"; fi; [[ -r "$__microcodex_rc" ]] && . "$__microcodex_rc"; unset __microcodex_rc; )";
        }
        if (name == "bash") {
            return R"(if [[ -z "$BASH_ENV" && -r "$HOME/.bashrc" ]]; then . "$HOME/.bashrc"; fi; )";
        }
        return R"(if [ -n "$ENV" ] && [ -r "$ENV" ]; then . "$ENV"; fi; )";
    }

    class UserShellContext {
    public:
        explicit UserShellContext(const std::stop_token stop_token) {
            const char *configured = std::getenv("SHELL");
            shell_ = configured != nullptr && configured[0] != '\0'
                         ? std::filesystem::path(configured)
                         : std::filesystem::path("/bin/sh");
            std::error_code error;
            if (!std::filesystem::is_regular_file(shell_, error)) {
                shell_ = "/bin/sh";
            }

            std::string pattern =
                (std::filesystem::temp_directory_path(error) /
                 "microcodex-shell-snapshot.XXXXXX")
                    .string();
            if (error) return;
            std::vector<char> writable(pattern.begin(), pattern.end());
            writable.push_back('\0');
            const int descriptor = mkstemp(writable.data());
            if (descriptor == -1) return;
            close(descriptor);
            snapshot_ = writable.data();

            const std::string snapshot_path = shellSingleQuote(snapshot_.string());
            const std::string script =
                shellRcPrefix(shell_) +
                "unset PWD OLDPWD; export -p > '" + snapshot_path + "'";
            try {
                const auto captured = runProcess(
                    {shell_.string(), "-lc", script}, stop_token,
                    std::chrono::steady_clock::now() + std::chrono::seconds(10));
                if (captured.error_code == 0 &&
                    std::filesystem::file_size(snapshot_, error) != 0 && !error) {
                    valid_snapshot_ = true;
                    return;
                }
            } catch (...) {
                // Shell startup is best-effort. The fallback below still uses
                // the user's login shell and sources its interactive rc file.
            }
            std::filesystem::remove(snapshot_, error);
            snapshot_.clear();
        }

        UserShellContext(const UserShellContext &) = delete;
        UserShellContext &operator=(const UserShellContext &) = delete;

        ~UserShellContext() {
            if (!snapshot_.empty()) {
                std::error_code error;
                std::filesystem::remove(snapshot_, error);
            }
        }

        std::vector<std::string> command(const std::string_view script) const {
            if (valid_snapshot_) {
                const std::string snapshot_path = shellSingleQuote(snapshot_.string());
                return {
                    shell_.string(),
                    "-c",
                    "if . '" + snapshot_path + "' >/dev/null 2>&1; then :; fi\n\n" +
                        std::string(script),
                };
            }
            return {
                shell_.string(),
                "-lc",
                shellRcPrefix(shell_) + std::string(script),
            };
        }

    private:
        std::filesystem::path shell_;
        std::filesystem::path snapshot_;
        bool valid_snapshot_ = false;
    };

    const UserShellContext &userShellContext(const std::stop_token stop_token) {
        // Static initialization is thread-safe. Passing the first caller's stop
        // token keeps the one-time snapshot capture interruptible as well.
        static const UserShellContext context(stop_token);
        return context;
    }

} // namespace

namespace microcodex {

    std::expected<BashCommandResult, std::string> bash(const std::string &cmd) {
        return bash(cmd, {});
    }

    std::expected<BashCommandResult, std::string> bash(const std::string &cmd, const std::stop_token stop_token) {
        try {
            if (cmd.empty()) {
                return std::unexpected("command must not be empty");
            }
            // Block lexically recognized destructive commands before starting the user's shell,
            // for example: `rm -rf path`, `git reset --hard`, `git clean -fd`, or `mkfs.ext4 device`.
            if (const auto reason = deniedBashCommandReason(cmd)) {
                return std::unexpected("command denied: " + std::string(*reason));
            }
            BashCommandResult result = runProcess(userShellContext(stop_token).command(cmd), stop_token);
            if (stop_token.stop_requested()) {
                return std::unexpected("Command interrupted");
            }
            return result;
        } catch (const std::exception &error) {
            return std::unexpected(error.what());
        }
    }

} // namespace microcodex
