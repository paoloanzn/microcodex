// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "bash.h"

#include <array>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstring>
#include <expected>
#include <poll.h>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

    std::expected<std::vector<std::string>, std::string> splitCmdOnWhiteSpaces(const std::string &cmd) {
        std::vector<std::string> arguments;
        std::string argument;
        bool building_argument = false;
        std::string::size_type position = 0;

        while (position < cmd.size()) {
            const char character = cmd[position++];

            if (std::isspace(static_cast<unsigned char>(character))) {
                if (building_argument) {
                    arguments.push_back(std::move(argument));
                    argument.clear();
                    building_argument = false;
                }
                continue;
            }

            building_argument = true;
            if (character == '\\') {
                if (position == cmd.size()) {
                    return std::unexpected("trailing escape character");
                }
                argument += cmd[position++];
                continue;
            }

            if (character != '\'' && character != '"') {
                argument += character;
                continue;
            }

            const char quote = character;
            bool quote_closed = false;
            while (position < cmd.size()) {
                const char quoted_character = cmd[position++];
                if (quoted_character == quote) {
                    quote_closed = true;
                    break;
                }
                if (quote == '"' && quoted_character == '\\') {
                    if (position == cmd.size()) {
                        return std::unexpected("trailing escape character");
                    }
                    argument += cmd[position++];
                } else {
                    argument += quoted_character;
                }
            }

            if (!quote_closed) {
                return std::unexpected(quote == '\'' ? "unterminated single quote" : "unterminated double quote");
            }
        }

        if (building_argument) {
            arguments.push_back(std::move(argument));
        }
        return arguments;
    }

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

    microcodex::BashCommandResult runProcess(const std::string &cmd, const std::stop_token stop_token) {
        if (stop_token.stop_requested()) {
            throw std::runtime_error("Command interrupted");
        }

        auto split_arguments = splitCmdOnWhiteSpaces(cmd);
        if (!split_arguments) {
            throw std::invalid_argument(split_arguments.error());
        }
        std::vector<std::string> arguments = std::move(*split_arguments);
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
        std::chrono::steady_clock::time_point interrupt_time;

        while (open_streams > 0 || !child_exited) {
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

        if (WIFEXITED(status)) {
            result.error_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            result.error_code = 128 + WTERMSIG(status);
        } else {
            result.error_code = -1;
        }
        return result;
    }

} // namespace

namespace microcodex {

    std::expected<BashCommandResult, std::string> bash(const std::string &cmd) {
        return bash(cmd, {});
    }

    std::expected<BashCommandResult, std::string> bash(const std::string &cmd, const std::stop_token stop_token) {
        try {
            BashCommandResult result = runProcess(cmd, stop_token);
            if (stop_token.stop_requested()) {
                return std::unexpected("Command interrupted");
            }
            return result;
        } catch (const std::exception &error) {
            return std::unexpected(error.what());
        }
    }

} // namespace microcodex
