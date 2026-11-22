// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

// Runs a command and writes its peak resident set size, in KiB, to
// RESULT_FILE. It exits with the command's status.
//
// The kernel counts the memory a process held before exec in that process's
// peak, so a measurement is never smaller than the RSS of whatever started
// the command. This helper stays around 1 MiB; an interpreter such as Ruby or
// Python would add its own 10-15 MiB, more than MicroCodex itself uses.
//
// Usage: peak-rss RESULT_FILE COMMAND [ARG...]

#define _DEFAULT_SOURCE  // wait4() on glibc

#include <errno.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fputs("usage: peak-rss RESULT_FILE COMMAND [ARG...]\n", stderr);
        return 2;
    }

    const pid_t pid = fork();
    if (pid == -1) {
        perror("peak-rss: fork");
        return 2;
    }
    if (pid == 0) {
        execvp(argv[2], &argv[2]);
        perror("peak-rss: exec");
        _exit(127);
    }

    // The usage covers the command and every child it waited for, such as the
    // shells the bash tool starts.
    int status = 0;
    struct rusage usage;
    while (wait4(pid, &status, 0, &usage) == -1) {
        if (errno != EINTR) {
            perror("peak-rss: wait4");
            return 2;
        }
    }

#ifdef __APPLE__
    const long peak_kib = usage.ru_maxrss / 1024;  // bytes on macOS
#else
    const long peak_kib = usage.ru_maxrss;  // KiB on Linux
#endif
    FILE *result = fopen(argv[1], "w");
    if (result == NULL || fprintf(result, "%ld\n", peak_kib) < 0 || fclose(result) != 0) {
        perror("peak-rss: cannot write the result");
        return 2;
    }

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + WTERMSIG(status);
}
