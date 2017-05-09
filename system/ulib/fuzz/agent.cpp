// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuzz/agent.h>

#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <fuzz/channel.h>
#include <launchpad/launchpad.h>
#include <magenta/errors.h>
#include <magenta/status.h>
#include <magenta/types.h>
#include <mx/channel.h>
#include <mx/object.h>
#include <mx/time.h>
#include <mxalloc/new.h>

namespace fuzz {

namespace {

// Max length of time to wait while reading handshake messages.
const uint32_t kHandshakeTimeout = MX_MSEC(500);

const size_t kMaxLineLen = 1024;

const char* kSentinel = "agent";

// Helper for converting POSIX errno's into mx_status_t's.
mx_status_t ToStatus(ssize_t rc) {
    if (rc >= 0) {
        return NO_ERROR;
    }
    if (rc == EPIPE) {
        return ERR_PEER_CLOSED;
    }
    if (rc == EIO) {
        return ERR_IO;
    }
    return ERR_INTERNAL;
}

} // namespace

// Public methods

Agent::~Agent() {}

mx_status_t Agent::Run(int argc, const char** argv, uint32_t timeout) {
    mx_status_t rc;
    // We must have an executable, and we limit ourselves to 255 parameters.
    if (argc == 0 || argc > UINT8_MAX || !argv) {
        return ERR_INVALID_ARGS;
    }
    argv0_ = argv[0];
    // Initialize the mutex and lock it before spawning the stdout and stderr
    // threads.  This will keep them blocked until the handshake is complete.
    // If we encounter an error, it is fine to release the lock as they will not
    // have a valid fd and will exit immediately.

    // Create the thread signaling event.
    if ((rc = mx::event::create(0, &ready_)) != NO_ERROR) {
        return rc;
    }
    ready_.signal(MX_USER_SIGNAL_ALL, MX_SIGNAL_NONE);
    // Create the stdout and stderr threads, which will initially wait for the
    // ready signal. On error, the event will be destroyed, waking those threads
    // with an error.
    if (thrd_create(&out_thrd_, Agent::HandleStdout, this) != thrd_success) {
        return ERR_NO_RESOURCES;
    }
    if (thrd_create(&err_thrd_, Agent::HandleStderr, this) != thrd_success) {
        return ERR_NO_RESOURCES;
    }
    // Create the channel and perform the handshake.
    mx_handle_t remote = MX_HANDLE_INVALID;
    if ((rc = fuzzer_.Listen(&remote)) != NO_ERROR ||
        (rc = Launch(argc, argv, remote)) != NO_ERROR ||
        (rc = Handshake(timeout)) != NO_ERROR) {
        return rc;
    }
    // Signal the threads and start processing state messages.
    ready_.signal(MX_SIGNAL_NONE, MX_EVENT_SIGNALED);
    return HandleState();
}

// Protected methods

Agent::Agent()
    : argv0_(nullptr), start_(0), stdin_(-1), stdout_(-1), stderr_(-1) {}

mx_status_t Agent::Launch(int argc, const char** argv, mx_handle_t remote) {
    launchpad_t* launchpad;
    // Create a process from the supplied arguments and channel.
    launchpad_create(MX_HANDLE_INVALID, argv[0], &launchpad);
    launchpad_load_from_file(launchpad, argv[0]);
    launchpad_set_args(launchpad, argc, argv);
    launchpad_add_handle(launchpad, remote, Channel::kHandleInfo);
    // Clone everything except stdio.  Set those up as pipes instead.
    launchpad_clone(launchpad, LP_CLONE_ALL & ~LP_CLONE_MXIO_STDIO);
    launchpad_add_pipe(launchpad, &stdin_, STDIN_FILENO);
    launchpad_add_pipe(launchpad, &stdout_, STDOUT_FILENO);
    launchpad_add_pipe(launchpad, &stderr_, STDERR_FILENO);
    // Launch!
    return launchpad_go(launchpad, proc_.get_address(), nullptr);
}

mx_status_t Agent::ToStdin(const char* in) {
    size_t len = strlen(in);
    if (len > INT_MAX) {
        return ERR_OUT_OF_RANGE;
    }
    return ToStatus(write(stdin_, in, strlen(in)));
}

// Private static methods (i.e. threads)

int Agent::HandleStdout(void* arg) {
    Agent* agent = static_cast<Agent*>(arg);
    return agent->HandleStdio(agent->stdout_);
}

int Agent::HandleStderr(void* arg) {
    Agent* agent = static_cast<Agent*>(arg);
    return agent->HandleStdio(agent->stderr_);
}

// Private methods

mx_status_t Agent::Handshake(mx_time_t timeout) {
    mx_status_t rc;
    fuzzer_.SetTimeout(kHandshakeTimeout);
    // Send the START message with the timeout.
    if ((rc = fuzzer_.Write(&timeout, sizeof(timeout))) != NO_ERROR) {
        return rc;
    }
    // Write the current state (may be empty).
    if ((rc = fuzzer_.Write(state_.get(), state_.size())) != NO_ERROR) {
        return rc;
    }
    // Read the initial state.
    if ((rc = fuzzer_.ReadBuf(&state_)) != NO_ERROR) {
        return rc;
    }
    fuzzer_.SetTimeout(timeout);
    return NO_ERROR;
}

mx_status_t Agent::HandleState() {
    mx_status_t rc;
    while ((rc = fuzzer_.ReadBuf(&state_)) == NO_ERROR) {
    }
    fuzzer_.Close();
    // Other end didn't respond; might have crashed
    if (rc == ERR_TIMED_OUT || rc == ERR_PEER_CLOSED) {
        return HandleCrash();
    }
    return rc;
}

mx_status_t Agent::HandleCrash() {
    mx_status_t rc;
    mx_info_process_t info;
    char buf[kMaxLineLen];
    if ((rc = proc_.get_info(MX_INFO_PROCESS, &info, sizeof(info), nullptr,
                             nullptr)) != NO_ERROR) {
        // Report mx_object_get_info failure.
        snprintf(buf, sizeof(buf),
                 "%s: unable to get process info for %s; "
                 "mx_object_get_info returned %s",
                 kSentinel, argv0_, mx_status_get_string(rc));
    } else if (!info.exited) {
        // Still running; must have timed out.
        mx_time_t duration =
            mx::time::get(MX_CLOCK_MONOTONIC) - fuzzer_.GetLast();
        mx_time_t timeout = fuzzer_.GetTimeout();
        snprintf(buf, sizeof(buf),
                 "%s: %s has not responded for %" PRIu64 ".%09" PRIu64
                 " seconds; timeout is %" PRIu64 ".%09" PRIu64,
                 kSentinel, argv0_, duration / MX_SEC(1), duration % MX_SEC(1),
                 timeout / MX_SEC(1), timeout % MX_SEC(1));
    } else if (info.return_code != 0) {
        // Only log non-zero exits
        snprintf(buf, sizeof(buf), "%s: %s exited with exit code %d", kSentinel,
                 argv0_, info.return_code);
    } else {
        // nothing to log.
        return NO_ERROR;
    }
    OnStderr(buf);
    return NO_ERROR;
}

mx_status_t Agent::HandleStdio(int fd) {
    mx_status_t rc;
    // Reserve space for buffering the I/O.
    char buf[kMaxLineLen + 1];
    buf[kMaxLineLen] = '\0';
    // Determine what method handles the output.
    void (Agent::*OnStdio)(const char* str);
    if (fd == stdout_) {
        OnStdio = &fuzz::Agent::OnStdout;
    } else if (fd == stderr_) {
        OnStdio = &fuzz::Agent::OnStderr;
    } else {
        return ERR_NOT_SUPPORTED;
    }
    // Wait for the handshake to complete.
    if ((rc = ready_.wait_one(MX_EVENT_SIGNALED, MX_TIME_INFINITE, nullptr)) !=
        NO_ERROR) {
        return rc;
    }
    // Loop, reading output from the fd.
    size_t len = 0;
    while (true) {
        ssize_t n = read(fd, buf + len, kMaxLineLen - len);
        if (n <= 0) {
            rc = ToStatus(n);
            break;
        }
        len += n;
        char last = buf[len - 1];
        // Tokenize the string into lines, and process each one.
        char* line = strtok(buf, "\n");
        char* next;
        while ((next = strtok(nullptr, "\n"))) {
            (this->*OnStdio)(line);
            line = next;
        }
        // If the buffer is full, flush it.  Otherwise, save the leftovers.
        len = strlen(line);
        if (last == '\0' || last == '\n' || len == kMaxLineLen) {
            (this->*OnStdio)(line);
            len = 0;
        } else {
            memmove(buf, line, len);
        }
    }
    // Flush whatever remains
    if (len > 0) {
        (this->*OnStdio)(buf);
    }
    return rc;
}

} // namespace fuzz
