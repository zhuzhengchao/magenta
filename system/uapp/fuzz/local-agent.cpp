// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuzz/agent.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <magenta/types.h>
#include <magenta/errors.h>

namespace fuzz {

class LocalAgent : public Agent {
protected:
    void OnStdout(const char* str) override {
        fprintf(stdout, "%s\n", str);
    }

    void OnStderr(const char* str) override {
        fprintf(stderr, "%s\n", str);
    }
};

} // namespace fuzz

namespace {

void Quit(const char *errfmt, ...) {
    if (errfmt) {
        va_list ap;
        printf("error: ");
        va_start(ap, errfmt);
        vprintf(errfmt, ap);
        va_end(ap);
        printf("\n");
    }
    printf("usage: local-agent [-t msecs] -- <fuzzer> <fuzzer-args>\n");
    exit(errfmt ? 1 : 0);
}

void ParseNVal(uint32_t* nval, const char* arg) {
    char* endptr = nullptr;
    unsigned long long value = strtoul(arg, &endptr, 0);
    if (arg[0] == '\0' || endptr[0] != '\0') {
        Quit("unable to parse number: %s", arg);
    }
    if (value > UINT32_MAX) {
        Quit("value is too large: %s", arg);
    }
    *nval = static_cast<uint32_t>(value);
}

} // namespace

int main(int argc, const char** argv) {
    fuzz::LocalAgent agent;
    uint32_t timeout = 5000;
    uint32_t *nval = nullptr;
    const char **sval = nullptr;
    // Consume our own name
    ++argv;
    --argc;
    while (argc != 0) {
        // Save the current argument and advance to the next.
        const char* arg = *argv;
        ++argv;
        --argc;
        if (nval) {
            // Numeric value requested.
            ParseNVal(nval, arg);
            nval = nullptr;
        } else if (sval) {
            // String value requested
            *sval = arg;
            sval = nullptr;
        } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "-?") == 0 ||
                   strcmp(arg, "--help") == 0) {
            // Print usage and exit!
            Quit(nullptr);
        } else if (strcmp(arg, "-t") == 0 || strcmp(arg, "--timeout") == 0) {
            // Queue the timeout as the next thing to parse.
            nval = &timeout;
        } else if (strcmp(arg, "--") == 0) {
            // The rest are fuzzer arguments
            break;
        } else {
            // Add more options here if needed...
            Quit("unknown option: %s", arg);
        }
    }
    return agent.Run(argc, argv, timeout);
}
