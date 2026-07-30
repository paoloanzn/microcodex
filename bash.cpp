// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#include "bash.h"

#include <cstdlib>
#include <expected>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

// Splits a command string into arguments while preserving quotes and escapes.
std::expected<std::vector<std::string>, std::string> splitCmdOnWhiteSpaces(
    const std::string& cmd
) {
    std::vector<std::string> arguments;
    std::string argument;
    bool building_argument = false;
    std::string::size_type position = 0;

    while (position < cmd.size()) {
        const char character = cmd[position++];

        if (std::isspace(static_cast<unsigned char>(character))) {
            // Whitespace ends the current unquoted argument.
            if (building_argument) {
                arguments.push_back(std::move(argument));
                argument.clear();
                building_argument = false;
            }
            continue;
        }

        building_argument = true;

        if (character == '\\') {
            // Escapes preserve the next character verbatim.
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

        // Collect quoted text as part of the current argument.
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
            return std::unexpected(
                quote == '\'' ? "unterminated single quote" : "unterminated double quote"
            );
        }
    }

    if (building_argument) {
        arguments.push_back(std::move(argument));
    }

    return arguments;
}

// Executes a command and captures its standard output and error streams.
microcodex::BashCommandResult runProcess(const std::string& cmd) {
    auto split_arguments = splitCmdOnWhiteSpaces(cmd);

    if (!split_arguments) {
        throw std::invalid_argument(split_arguments.error());
    }

    std::vector<std::string> arguments = std::move(split_arguments.value());

    if (arguments.empty()) {
        throw std::invalid_argument("command must not be empty");
    }

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);

    for (const std::string& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }

    // execvp() requires a null-terminated argument vector.
    argv.push_back(nullptr);

    int stdout_pipe[2];
    int stderr_pipe[2];

    // Capture both streams independently to avoid interleaving their output.
    if (pipe(stdout_pipe) == -1 || pipe(stderr_pipe) == -1) {
        throw std::runtime_error(
            std::string("pipe failed: ") + std::strerror(errno)
        );
    }

    const pid_t pid = fork();

    if (pid == -1) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);

        throw std::runtime_error(
            std::string("fork failed: ") + std::strerror(errno)
        );
    }

    if (pid == 0) {
        // Child process.

        close(stdout_pipe[0]);
        close(stderr_pipe[0]);

        if (dup2(stdout_pipe[1], STDOUT_FILENO) == -1 ||
            dup2(stderr_pipe[1], STDERR_FILENO) == -1) {
            _exit(126);
        }

        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        execvp(argv[0], argv.data());

        // execvp() returns only on failure.
        const char* message = std::strerror(errno);
        write(STDERR_FILENO, message, std::strlen(message));
        write(STDERR_FILENO, "\n", 1);
        _exit(127);
    }

    // Parent process.

    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    microcodex::BashCommandResult result{};
    std::array<char, 4096> buffer{};

    pollfd descriptors[2]{
        {stdout_pipe[0], POLLIN, 0},
        {stderr_pipe[0], POLLIN, 0}
    };

    int open_streams = 2;

    // Drain both pipes concurrently so either stream cannot block the child.
    while (open_streams > 0) {
        const int poll_result = poll(descriptors, 2, -1);

        if (poll_result == -1) {
            if (errno == EINTR) {
                continue;
            }

            close(stdout_pipe[0]);
            close(stderr_pipe[0]);

            throw std::runtime_error(
                std::string("poll failed: ") + std::strerror(errno)
            );
        }

        for (std::size_t i = 0; i < 2; ++i) {
            pollfd& descriptor = descriptors[i];

            if (descriptor.fd == -1) {
                continue;
            }

            if (descriptor.revents & (POLLIN | POLLHUP)) {
                const ssize_t count =
                    read(descriptor.fd, buffer.data(), buffer.size());

                if (count > 0) {
                    std::string& destination =
                        i == 0 ? result.stdout : result.stderr;

                    destination.append(
                        buffer.data(),
                        static_cast<std::size_t>(count)
                    );
                } else if (count == 0) {
                    close(descriptor.fd);
                    descriptor.fd = -1;
                    --open_streams;
                } else if (errno != EINTR) {
                    close(descriptor.fd);
                    descriptor.fd = -1;
                    --open_streams;
                }
            }

            if (descriptor.revents & (POLLERR | POLLNVAL)) {
                close(descriptor.fd);
                descriptor.fd = -1;
                --open_streams;
            }
        }
    }

    int status = 0;

    // Reap the child, retrying if a signal interrupts the wait.
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            throw std::runtime_error(
                std::string("waitpid failed: ") + std::strerror(errno)
            );
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

    std::expected<BashCommandResult, std::string> bash(const std::string& cmd) {
        // here we might want to gate dangerous commands
        // example: rm -rf, :(){ :|:& };: etc...
        try {
            return runProcess(cmd);
        } catch (const std::exception& error) {
            return std::unexpected(error.what());
        }
    }

} // namespace microcodex
