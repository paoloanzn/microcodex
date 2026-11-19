// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "bash.h"

#include <poll.h>    // IWYU pragma: keep
#include <signal.h>  // NOLINT(modernize-deprecated-headers): POSIX declares kill here.
#include <stdlib.h>  // NOLINT(modernize-deprecated-headers): POSIX declares mkstemp here.
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "bash-safety.h"

namespace {

void closeDescriptor(int& descriptor) {
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
    while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
}

std::string systemError(const std::string_view operation, const int error_number) {
    return std::string(operation) + ": " + std::generic_category().message(error_number);
}

using PipeDescriptors = std::array<int, 2>;
constexpr nfds_t stream_count = 2;

void createPipes(PipeDescriptors& stdout_pipe, PipeDescriptors& stderr_pipe) {
    if (pipe(stdout_pipe.data()) == -1) {
        throw std::runtime_error(systemError("pipe failed", errno));
    }
    if (pipe(stderr_pipe.data()) == -1) {
        const int error_number = errno;
        closeDescriptor(stdout_pipe[0]);
        closeDescriptor(stdout_pipe[1]);
        throw std::runtime_error(systemError("pipe failed", error_number));
    }
}

// Runs in the forked child: redirects output into the pipes and execs the
// command. Only async-signal-safe calls are allowed here.
[[noreturn]] void execChild(PipeDescriptors& stdout_pipe, PipeDescriptors& stderr_pipe, std::vector<char*>& argv) {
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

    execvp(argv.front(), argv.data());

    // execvp() returns only on failure. Use the C system call here
    // because C++ streams are not safe after fork in a multithreaded process.
    // NOLINTNEXTLINE(concurrency-mt-unsafe): the forked child runs a single thread and must not allocate.
    const char* message = std::strerror(errno);
    // Nothing can report a failed write from here, so the results are dropped.
    [[maybe_unused]] const ssize_t message_written = ::write(STDERR_FILENO, message, std::strlen(message));
    [[maybe_unused]] const ssize_t newline_written = ::write(STDERR_FILENO, "\n", 1);
    _exit(127);
}

// Everything the parent tracks while it waits for the child and drains its
// stdout and stderr pipes.
struct ChildProcess {
    pid_t pid = -1;
    std::array<pollfd, stream_count> descriptors{};
    int open_streams = 2;
    int status = 0;
    bool exited = false;
    bool sent_interrupt = false;
    bool sent_kill = false;
    bool timed_out = false;
    std::chrono::steady_clock::time_point interrupt_time;
};

// Kills the child at the deadline, interrupts it on stop, and escalates an
// interrupt that has been ignored for a second.
void signalChild(ChildProcess& child, const std::stop_token& stop_token,
                 const std::optional<std::chrono::steady_clock::time_point>& deadline) {
    if (child.exited) return;
    if (deadline && std::chrono::steady_clock::now() >= *deadline && !child.sent_kill) {
        signalProcess(child.pid, SIGKILL);
        child.sent_kill = true;
        child.timed_out = true;
    }
    if (stop_token.stop_requested() && !child.sent_interrupt) {
        signalProcess(child.pid, SIGINT);
        child.sent_interrupt = true;
        child.interrupt_time = std::chrono::steady_clock::now();
    }
    if (child.sent_interrupt && !child.sent_kill &&
        std::chrono::steady_clock::now() - child.interrupt_time >= std::chrono::seconds(1)) {
        // SIGINT gives well-behaved commands time to clean up. SIGKILL
        // guarantees that interrupt() cannot wait forever otherwise.
        signalProcess(child.pid, SIGKILL);
        child.sent_kill = true;
    }
}

void reapChild(ChildProcess& child) {
    if (child.exited) return;
    const pid_t wait_result = waitpid(child.pid, &child.status, WNOHANG);
    if (wait_result == child.pid) {
        child.exited = true;
    } else if (wait_result == -1 && errno != EINTR) {
        const int error_number = errno;
        closeDescriptor(child.descriptors[0].fd);
        closeDescriptor(child.descriptors[1].fd);
        throw std::runtime_error(systemError("waitpid failed", error_number));
    }
}

// Waits briefly for output. Returns false when there is nothing to read yet.
bool waitForOutput(ChildProcess& child) {
    if (child.open_streams == 0) {
        // poll() with no live descriptors provides a short cancellable
        // wait while the process finishes after closing its streams.
        poll(nullptr, 0, 50);
        return false;
    }

    const int poll_result = poll(child.descriptors.data(), stream_count, 100);
    if (poll_result == -1) {
        if (errno == EINTR) {
            return false;
        }
        const int error_number = errno;
        closeDescriptor(child.descriptors[0].fd);
        closeDescriptor(child.descriptors[1].fd);
        killAndReap(child.pid);
        throw std::runtime_error(systemError("poll failed", error_number));
    }
    return true;
}

void readStream(pollfd& descriptor, std::string& destination, const std::span<char> buffer, int& open_streams) {
    if (descriptor.fd == -1) {
        return;
    }

    if ((descriptor.revents & (POLLIN | POLLHUP)) != 0) {
        const ssize_t count = ::read(descriptor.fd, buffer.data(), buffer.size());
        if (count > 0) {
            destination.append(buffer.data(), static_cast<std::size_t>(count));
        } else if (count == 0 || errno != EINTR) {
            closeDescriptor(descriptor.fd);
            --open_streams;
        }
    }

    if (descriptor.fd != -1 && (descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
        closeDescriptor(descriptor.fd);
        --open_streams;
    }
}

// status stays non-const: macOS's W* macros cast its address to int*.
int exitCode(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return -1;
}

microcodex::BashCommandResult runProcess(
    const std::vector<std::string>& arguments, const std::stop_token stop_token,
    const std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt) {
    if (stop_token.stop_requested()) {
        throw std::runtime_error("Command interrupted");
    }
    if (arguments.empty()) {
        throw std::invalid_argument("command must not be empty");
    }

    // execvp() takes mutable strings, so it gets its own copies.
    std::vector<std::string> writable_arguments = arguments;
    std::vector<char*> argv;
    argv.reserve(writable_arguments.size() + 1);
    for (std::string& argument : writable_arguments) {
        argv.push_back(argument.data());
    }
    // execvp() requires a null-terminated argument vector.
    argv.push_back(nullptr);

    PipeDescriptors stdout_pipe{-1, -1};
    PipeDescriptors stderr_pipe{-1, -1};
    createPipes(stdout_pipe, stderr_pipe);

    const pid_t pid = fork();
    if (pid == -1) {
        const int error_number = errno;
        closeDescriptor(stdout_pipe[0]);
        closeDescriptor(stdout_pipe[1]);
        closeDescriptor(stderr_pipe[0]);
        closeDescriptor(stderr_pipe[1]);
        throw std::runtime_error(systemError("fork failed", error_number));
    }
    if (pid == 0) {
        execChild(stdout_pipe, stderr_pipe, argv);
    }

    closeDescriptor(stdout_pipe[1]);
    closeDescriptor(stderr_pipe[1]);
    // The child also calls setpgid(); doing it in the parent closes the
    // short race before the child reaches that line. EACCES/ESRCH are safe
    // here because the child has either exec'd or already exited.
    setpgid(pid, pid);

    microcodex::BashCommandResult result{};
    std::array<char, 4096> buffer{};
    ChildProcess child{
        .pid = pid,
        .descriptors = {{{.fd = stdout_pipe[0], .events = POLLIN, .revents = 0},
                         {.fd = stderr_pipe[0], .events = POLLIN, .revents = 0}}},
    };

    while (child.open_streams > 0 || !child.exited) {
        signalChild(child, stop_token, deadline);
        reapChild(child);
        if (!waitForOutput(child)) {
            continue;
        }
        readStream(child.descriptors[0], result.stdout, buffer, child.open_streams);
        readStream(child.descriptors[1], result.stderr, buffer, child.open_streams);
    }

    if (child.timed_out) {
        throw std::runtime_error("Command timed out");
    }
    result.error_code = exitCode(child.status);
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

std::string shellRcPrefix(const std::filesystem::path& shell) {
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
        const char* configured = std::getenv("SHELL");
        const std::string_view configured_shell = configured != nullptr ? configured : "";
        shell_ = configured_shell.empty() ? std::filesystem::path("/bin/sh") : std::filesystem::path(configured_shell);
        std::error_code error;
        if (!std::filesystem::is_regular_file(shell_, error)) {
            shell_ = "/bin/sh";
        }

        std::string pattern =
            (std::filesystem::temp_directory_path(error) / "microcodex-shell-snapshot.XXXXXX").string();
        if (error) return;
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const int descriptor = mkstemp(writable.data());
        if (descriptor == -1) return;
        close(descriptor);
        snapshot_ = writable.data();

        const std::string snapshot_path = shellSingleQuote(snapshot_.string());
        const std::string script = shellRcPrefix(shell_) + "unset PWD OLDPWD; export -p > '" + snapshot_path + "'";
        if (captureSnapshot(script, stop_token)) {
            valid_snapshot_ = true;
            return;
        }
        std::filesystem::remove(snapshot_, error);
        snapshot_.clear();
    }

    UserShellContext(const UserShellContext&) = delete;
    UserShellContext& operator=(const UserShellContext&) = delete;
    UserShellContext(UserShellContext&&) = delete;
    UserShellContext& operator=(UserShellContext&&) = delete;

    ~UserShellContext() {
        if (!snapshot_.empty()) {
            std::error_code error;
            std::filesystem::remove(snapshot_, error);
        }
    }

    [[nodiscard]] std::vector<std::string> command(const std::string_view script) const {
        if (valid_snapshot_) {
            const std::string snapshot_path = shellSingleQuote(snapshot_.string());
            return {
                shell_.string(),
                "-c",
                "if . '" + snapshot_path + "' >/dev/null 2>&1; then :; fi\n\n" + std::string(script),
            };
        }
        return {
            shell_.string(),
            "-lc",
            shellRcPrefix(shell_) + std::string(script),
        };
    }

private:
    // Runs the login shell once to export its environment into snapshot_.
    // Shell startup is best-effort: on any failure the caller falls back to
    // the user's login shell, which still sources its interactive rc file.
    [[nodiscard]] bool captureSnapshot(const std::string& script, const std::stop_token stop_token) const {
        try {
            const auto captured = runProcess({shell_.string(), "-lc", script}, stop_token,
                                             std::chrono::steady_clock::now() + std::chrono::seconds(10));
            std::error_code error;
            return captured.error_code == 0 && std::filesystem::file_size(snapshot_, error) != 0 && !error;
        } catch (...) {
            return false;
        }
    }

    std::filesystem::path shell_;
    std::filesystem::path snapshot_;
    bool valid_snapshot_ = false;
};

const UserShellContext& userShellContext(const std::stop_token stop_token) {
    // Static initialization is thread-safe. Passing the first caller's stop
    // token keeps the one-time snapshot capture interruptible as well.
    static const UserShellContext context(stop_token);
    return context;
}

}  // namespace

namespace microcodex {

std::expected<BashCommandResult, std::string> bash(const std::string& cmd) { return bash(cmd, {}); }

std::expected<BashCommandResult, std::string> bash(const std::string& cmd, const std::stop_token stop_token) {
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
    } catch (const std::exception& error) {
        return std::unexpected(error.what());
    }
}

}  // namespace microcodex
