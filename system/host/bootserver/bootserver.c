// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <errno.h>
#include <stdint.h>

#include <magenta/boot/netboot.h>

#define DEFAULT_US_BETWEEN_PACKETS 20
#define MAX_US_BETWEEN_PACKETS 2500

// The resend_request table is used to keep track of packets that weren't received
#define RRT_MAX_SIZE 1000000
static uint32_t resend_request_table[RRT_MAX_SIZE];
static size_t rrt_entries = 0;  // Actual table size (file sz/packet sz)

static uint32_t cookie = 1;
static char* appname;
static struct in6_addr allowed_addr;
static const char spinner[] = {'|', '/', '-', '\\'};
static const int MAX_READ_RETRIES = 10;
static const int MAX_SEND_RETRIES = 10000;
static int64_t us_between_packets = DEFAULT_US_BETWEEN_PACKETS;

static int io_rcv(int s, nbmsg* msg, nbmsg* ack, bool quiet) {
    for (int i = 0; i < MAX_READ_RETRIES; i++) {
        bool retry_allowed = i + 1 < MAX_READ_RETRIES;

        int r = read(s, ack, 2048);
        if (r < 0) {
            if (retry_allowed && errno == EAGAIN) {
                continue;
            }
            if (!quiet)
                fprintf(stderr, "\n%s: error: Socket read error %d\n", appname, errno);
            return -1;
        }
        if (r < sizeof(nbmsg)) {
            if (!quiet)
                fprintf(stderr, "\n%s: error: Read too short\n", appname);
            return -1;
        }
#ifdef DEBUG
        fprintf(stdout, " < magic = %08x, cookie = %08x, cmd = %08x, arg = %08x\n",
                ack->magic, ack->cookie, ack->cmd, ack->arg);
#endif

        if (ack->magic != NB_MAGIC) {
            if (!quiet)
                fprintf(stderr, "\n%s: error: Bad magic - ignoring\n", appname);
            return 0;
        }

        switch (ack->cmd) {
        case NB_RESEND:
            {
                size_t num_entries = (r - sizeof(nbmsg)) / sizeof(uint32_t);
                if ((rrt_entries + num_entries) > RRT_MAX_SIZE)
                    num_entries = RRT_MAX_SIZE - rrt_entries;
                memcpy(&resend_request_table[rrt_entries], &ack->data,
                       num_entries * sizeof(uint32_t));
                rrt_entries += num_entries;
                return 0;
            }
        case NB_ACK:
        case NB_RESEND_DONE:
        case NB_FILE_RECEIVED:
            return 0;
        case NB_ERROR:
            if (!quiet)
                fprintf(stderr, "\n%s: error: Generic error\n", appname);
            break;
        case NB_ERROR_BAD_CMD:
            if (!quiet)
                fprintf(stderr, "\n%s: error: Bad command\n", appname);
            break;
        case NB_ERROR_BAD_PARAM:
            if (!quiet)
                fprintf(stderr, "\n%s: error: Bad parameter\n", appname);
            break;
        case NB_ERROR_TOO_LARGE:
            if (!quiet)
                fprintf(stderr, "\n%s: error: File too large\n", appname);
            break;
        case NB_ERROR_BAD_FILE:
            if (!quiet)
                fprintf(stderr, "\n%s: error: Bad file\n", appname);
            break;
        default:
            if (!quiet)
                fprintf(stderr, "\n%s: error: Unknown command 0x%08X\n", appname, ack->cmd);
        }
        return -1;
    }
    if (!quiet)
        fprintf(stderr, "\n%s: error: Unexpected code path\n", appname);
    return -1;
}

static int io_send(int s, nbmsg* msg, size_t len, bool quiet) {
    for (int i = 0; i < MAX_SEND_RETRIES; i++) {
#if defined(__APPLE__)
        bool retry_allowed = i + 1 < MAX_SEND_RETRIES;
#endif

        int r = write(s, msg, len);
        if (r < 0) {
#if defined(__APPLE__)
            if (retry_allowed && errno == ENOBUFS) {
                // On Darwin we manage to overflow the ethernet driver, so retry
                struct timespec reqtime;
                reqtime.tv_sec = 0;
                reqtime.tv_nsec = 50 * 1000;
                nanosleep(&reqtime, NULL);
                continue;
            }
#endif
            if (!quiet)
                fprintf(stderr, "\n%s: error: Socket write error %d\n", appname, errno);
            return -1;
        }
        return 0;
    }
    if (!quiet)
        fprintf(stderr, "\n%s: error: Unexpected code path\n", appname);
    return -1;
}

#define ACK_RESEND_TIME 100000 // 0.1 secs
#define ACK_MAX_REPEATS 100

// There are three primary use cases for this routine:
// 1. |msg| is set, and |wait_reply| = true:
//    Send |msg| and wait for a response, repeating |msg| every ACK_RESEND_TIME
//    useconds.
// 2. |msg| is set, and |wait_reply| = false:
//    Send |msg| and process a pending response, if there is one.
// 3. |msg| is not set:
//    Wait for, and process, a message from the target.
// In all cases, |ack| is set to the received message, so the caller can
// identify and handle synchronous messages.
static bool io(int s, nbmsg* msg, size_t len, nbmsg* ack, bool wait_reply, bool quiet) {
    int n = s + 1;
    struct timeval tv;
    fd_set reads, writes;
    fd_set* ws = NULL;
    fd_set* rs = NULL;
    bool msg_sent = false;
    unsigned int repeats_left = ACK_MAX_REPEATS;

    ack->cookie = 0;
    ack->cmd = 0;
    ack->arg = 0;

    do {
        FD_ZERO(&reads);
        FD_SET(s, &reads);
        rs = &reads;

        if (msg && !msg_sent) {
            FD_ZERO(&writes);
            FD_SET(s, &writes);
            ws = &writes;
            msg->magic = NB_MAGIC;
            msg->cookie = cookie++;
        }

        tv.tv_sec = 0;
        tv.tv_usec = ACK_RESEND_TIME;
        int rv = select(n, rs, ws, NULL, &tv);
        if (rv < 0) {
            if (!quiet)
                fprintf(stderr, "\n%s: error: Select failed %d\n", appname, errno);
            return false;
        } else if (rv > 0) {
            if (FD_ISSET(s, &reads)) {
                if (io_rcv(s, msg, ack, quiet) != 0)
                    return false;
                if (!msg || msg_sent)
                    return true;
            }
            // Send the message for the first time
            if (msg && !msg_sent && FD_ISSET(s, &writes)) {
                if (io_send(s, msg, len, quiet) != 0)
                    return false;
                if (!wait_reply)
                    return true;
                msg_sent = true;
                ws = NULL;
            }
        } else {
            // Timeout - try sending the message again
            if (io_send(s, msg, len, quiet) != 0)
                return false;
        }
    } while (repeats_left-- > 0);

    // Timed-out
    if (!quiet)
        fprintf(stderr, "\n%s: error: No response received from target - timed out\n", appname);
    return false;
}

typedef struct {
    FILE* fp;
    const char* data;
    const char* next;
    size_t datalen;
} xferdata;

static ssize_t xread(xferdata* xd, void* data, size_t len) {
    if (xd->fp == NULL) {
        size_t bytes_remaining = xd->datalen - (xd->next - xd->data);
        if (len > bytes_remaining) {
            len = bytes_remaining;
        }
        memcpy(data, xd->next, len);
        xd->data += len;
        return len;
    } else {
        ssize_t r = fread(data, 1, len, xd->fp);
        if (r == 0) {
            return ferror(xd->fp) ? -1 : 0;
        }
        return r;
    }
}

static ssize_t xread_with_offset(xferdata *xd, void *data, size_t offset, size_t len) {
    if (xd->fp == NULL)
        xd->next = xd->data + offset;
    else
        if (fseek(xd->fp, offset, SEEK_SET) != 0)
            return -1;
    return xread(xd, data, len);
}

// UDP6_MAX_PAYLOAD (ETH_MTU - ETH_HDR_LEN - IP6_HDR_LEN - UDP_HDR_LEN)
//      1452           1514   -     14      -     40      -    8
// nbfile is PAYLOAD_SIZE + 2 * sizeof(size_t)

// Some EFI network stacks have problems with larger packets
// 1280 is friendlier
#define PAYLOAD_SIZE 1280

static int xfer(struct sockaddr_in6* addr, const char* fn, const char* name, bool boot) {
    xferdata xd;
    char msgbuf[2048];
    char ackbuf[2048];
    char tmp[INET6_ADDRSTRLEN];
    struct timeval tv;
    struct timeval begin, end;
    nbmsg* msg = (void*)msgbuf;
    nbmsg* ack = (void*)ackbuf;
    int s, r;
    int count = 0, spin = 0;
    size_t current_pos = 0;
    int64_t curr_packet_delay = us_between_packets;
    bool completed = false;

    // This only works on POSIX systems
    bool is_redirected = !isatty(fileno(stdout));

    if (!strcmp(fn, "(cmdline)")) {
        xd.fp = NULL;
        xd.data = name;
        xd.next = xd.data;
        xd.datalen = strlen(name) + 1;
        name = "cmdline";
    } else if ((xd.fp = fopen(fn, "rb")) == NULL) {
        fprintf(stderr, "%s: error: Could not open file %s\n", appname, fn);
        return -1;
    }

    long sz = 0;
    if (xd.fp) {
        if (fseek(xd.fp, 0L, SEEK_END)) {
            fprintf(stderr, "%s: error: Could not determine size of %s\n", appname, fn);
            return -1;
        } else if ((sz = ftell(xd.fp)) < 0) {
            fprintf(stderr, "%s: error: Could not determine size of %s\n", appname, fn);
            return -1;
        } else if (fseek(xd.fp, 0L, SEEK_SET)) {
            fprintf(stderr, "%s: error: Failed to rewind %s\n", appname, fn);
            return -1;
        }
    }

    if ((s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        fprintf(stderr, "%s: error: Cannot create socket %d\n", appname, errno);
        goto done;
    }
    fprintf(stderr, "%s: sending '%s'... (%ld bytes)\n", appname, fn, sz);
    gettimeofday(&begin, NULL);
    tv.tv_sec = 0;
    tv.tv_usec = 250 * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(s, (void*)addr, sizeof(*addr)) < 0) {
        fprintf(stderr, "%s: error: Cannot connect to [%s]%d\n", appname,
                inet_ntop(AF_INET6, &addr->sin6_addr, tmp, sizeof(tmp)),
                ntohs(addr->sin6_port));
        goto done;
    } else {
        fprintf(stderr, "%s: Connected on [%s]%d\n", appname,
                inet_ntop(AF_INET6, &addr->sin6_addr, tmp, sizeof(tmp)),
                ntohs(addr->sin6_port));
    }
    msg->cmd = NB_SEND_FILE;
    msg->arg = sz;
    strcpy((void*)msg->data, name);
    do {
        if (!io(s, msg, sizeof(nbmsg) + strlen(name) + 1, ack, true, false)) {
            fprintf(stderr, "%s: error: Failed to start transfer\n", appname);
            goto done;
        }
    } while (ack->cmd != NB_ACK);

    msg->cmd = NB_DATA;
    msg->arg = 0;

    bool resend_in_progress = false;
    size_t offset = 0;
    size_t packets_to_be_sent = (sz + (PAYLOAD_SIZE - 1)) / PAYLOAD_SIZE;

    if (packets_to_be_sent == 0) {
        completed = true;
        goto done;
    }

    float progress;
    int iterations = 1;

    fprintf(stderr, "%s: Pass %d (delay = %ldus):\n",
            appname, iterations, (long)curr_packet_delay);

    do {
        struct timeval packet_start_time;
        gettimeofday(&packet_start_time, NULL);

        if (resend_in_progress) {
            size_t packets_sent = packets_to_be_sent - rrt_entries;
            progress = 100.0 * ((float)packets_sent / (float)packets_to_be_sent);
            if (rrt_entries) {
                offset = resend_request_table[--rrt_entries] * PAYLOAD_SIZE;
                r = xread_with_offset(&xd, msg->data, offset, PAYLOAD_SIZE);
            } else {
                r = 0;
            }
        } else {
            progress = 100.0 * ((float)offset / (float)sz);
            r = xread(&xd, msg->data, PAYLOAD_SIZE);
        }
        if (r < 0) {
            fprintf(stderr, "\n%s: error: Reading '%s'\n", appname, fn);
            goto done;
        }

        if (is_redirected) {
            if (count++ > 8 * 1024) {
                fprintf(stderr, "%.01f%%\n", progress);
                count = 0;
            }
        } else {
            if (count++ > 1024 || r == 0) {
                count = 0;
                fprintf(stderr, "\33[2K\r%c %.01f%%",
                        spinner[(spin++) % 4], progress);
            }
        }

        if (r == 0) {
            fprintf(stderr, "\n%s: Reached end of file, waiting for confirmation.\n",
                    appname);
            msg->cmd = NB_LAST_DATA;
            msg->arg = 0;
            rrt_entries = 0;
            if (!io(s, msg, sizeof(nbmsg), ack, true, false) ||
                (ack->cmd != NB_FILE_RECEIVED &&
                 ack->cmd != NB_RESEND &&
                 ack->cmd != NB_RESEND_DONE))
                goto done;
            if (ack->cmd == NB_FILE_RECEIVED) {
                completed = true;
            } else {
                fprintf(stderr, "%s: Confirmation received, getting dropped packet info...\n",
                        appname);
                while (ack->cmd != NB_RESEND_DONE) {
                    if (!io(s, NULL, 0, ack, true, false))
                        goto done;
                };
                resend_in_progress = true;
                size_t dropped_packets = ack->arg;
                float loss = (float)dropped_packets / (float)packets_to_be_sent;
                fprintf(stderr, "%s: %d of %d packets were reported dropped (%.2f%%)\n",
                        appname, (int)dropped_packets, (int)packets_to_be_sent,
                        loss * 100.0);
                float new_packet_delay = (float)curr_packet_delay;
                new_packet_delay *= (1.0 + loss);
                if ((int64_t) new_packet_delay == curr_packet_delay) {
                    curr_packet_delay++;
                } else {
                    curr_packet_delay = (int64_t)new_packet_delay;
                }
                if (curr_packet_delay > MAX_US_BETWEEN_PACKETS)
                    curr_packet_delay = MAX_US_BETWEEN_PACKETS;
                fprintf(stderr, "%s: Pass %d (delay = %ldus):\n",
                        appname, ++iterations, (long)curr_packet_delay);
                if (rrt_entries != 0)
                    packets_to_be_sent = rrt_entries;
                gettimeofday(&packet_start_time, NULL);
            }
        } else {
            msg->cmd = NB_DATA;
            msg->arg = offset;

            if (!io(s, msg, sizeof(nbmsg) + r, ack, false, false)) {
                goto done;
            }

            if (ack->cmd == NB_FILE_RECEIVED) {
                goto done;
            }
        }

        // Some UEFI netstacks can lose back-to-back packets at max speed
        // so throttle output.
        // At 1280 bytes per packet, we should at least have 10 microseconds
        // between packets, to be safe using 20 microseconds here.
        // 1280 bytes * (1,000,000/10) seconds = 128,000,000 bytes/seconds = 122MB/s = 976Mb/s
        // We wait as a busy wait as the context switching a sleep can cause
        // will often degrade performance significantly.
        int64_t us_since_last_packet;
        do {
            struct timeval now;
            gettimeofday(&now, NULL);
            us_since_last_packet = (int64_t)(now.tv_sec - packet_start_time.tv_sec) * 1000000 +
                                   ((int64_t)now.tv_usec - (int64_t)packet_start_time.tv_usec);
        } while (us_since_last_packet < curr_packet_delay);

        if (!resend_in_progress) {
            offset += r;
            current_pos += r;
        }
    } while (!completed);

    if (boot) {
        msg->cmd = NB_BOOT;
        msg->arg = 0;
        fprintf(stderr, "%s: Sending boot command\n", appname);
        // We expect this to fail when the system starts to boot
        io(s, msg, sizeof(nbmsg), ack, true, true);
    } else {
        fprintf(stderr, "\n");
    }
done:
    if (completed) {
        gettimeofday(&end, NULL);
        if (end.tv_usec < begin.tv_usec) {
            end.tv_sec -= 1;
            end.tv_usec += 1000000;
        }
        fprintf(stderr, "%s: %s %ldMB %d.%06d sec\n\n", appname,
                fn, current_pos / (1024 * 1024), (int)(end.tv_sec - begin.tv_sec),
                (int)(end.tv_usec - begin.tv_usec));
    }
    if (s >= 0) {
        close(s);
    }
    if (xd.fp != NULL) {
        fclose(xd.fp);
    }
    return completed ? 0 : -1;
}

void usage(void) {
    fprintf(stderr,
            "usage:   %s [ <option> ]* <kernel> [ <ramdisk> ] [ -- [ <kerneloption> ]* ]\n"
            "\n"
            "options:\n"
            "  -1      only boot once, then exit\n"
            "  -a      only boot device with this IPv6 address\n"
            "  -i <NN> initial setting for number of microseconds between packets\n"
            "          set between 50-500 to deal with poor bootloader network stacks (default=%d)\n"
            "  -n      only boot device with this nodename\n",
            appname, DEFAULT_US_BETWEEN_PACKETS);
    exit(1);
}

void drain(int fd) {
    char buf[4096];
    if (fcntl(fd, F_SETFL, O_NONBLOCK) == 0) {
        while (read(fd, buf, sizeof(buf)) > 0)
            ;
        fcntl(fd, F_SETFL, 0);
    }
}

int main(int argc, char** argv) {
    struct sockaddr_in6 addr;
    char tmp[INET6_ADDRSTRLEN];
    char cmdline[4096];
    char* cmdnext = cmdline;
    char* nodename = NULL;
    int r, s, n = 1;
    const char* kernel_fn = NULL;
    const char* ramdisk_fn = NULL;
    int once = 0;
    int status;

    cmdline[0] = 0;
    if ((appname = strrchr(argv[0], '/')) != NULL) {
        appname++;
    } else {
        appname = argv[0];
    }

    while (argc > 1) {
        if (argv[1][0] != '-') {
            if (kernel_fn == NULL) {
                kernel_fn = argv[1];
            } else if (ramdisk_fn == NULL) {
                ramdisk_fn = argv[1];
            } else {
                usage();
            }
        } else if (!strcmp(argv[1], "-1")) {
            once = 1;
        } else if (!strcmp(argv[1], "-i")) {
            if (argc <= 1) {
                fprintf(stderr, "'-i' option requires an argument (micros between packets)\n");
                return -1;
            }
            errno = 0;
            us_between_packets = strtoll(argv[2], NULL, 10);
            if (errno != 0 || us_between_packets <= 0) {
                fprintf(stderr, "invalid arg for -i: %s\n", argv[2]);
                return -1;
            }
            fprintf(stderr, "initial packet spacing set to %" PRId64 " microseconds\n", us_between_packets);
            argc--;
            argv++;
        } else if (!strcmp(argv[1], "-a")) {
            if (argc <= 1) {
                fprintf(stderr, "'-a' option requires a valid ipv6 address\n");
                return -1;
            }
            if (inet_pton(AF_INET6, argv[2], &allowed_addr) != 1) {
                fprintf(stderr, "%s: invalid ipv6 address specified\n", argv[2]);
                return -1;
            }
            argc--;
            argv++;
        } else if (!strcmp(argv[1], "-n")) {
            if (argc <= 1) {
                fprintf(stderr, "'-n' option requires a valid nodename\n");
                return -1;
            }
            nodename = argv[2];
            argc--;
            argv++;
        } else if (!strcmp(argv[1], "--")) {
            while (argc > 2) {
                size_t len = strlen(argv[2]);
                if (len > (sizeof(cmdline) - 2 - (cmdnext - cmdline))) {
                    fprintf(stderr, "%s: commandline too large\n", appname);
                    return -1;
                }
                if (cmdnext != cmdline) {
                    *cmdnext++ = ' ';
                }
                memcpy(cmdnext, argv[2], len + 1);
                cmdnext += len;
                argc--;
                argv++;
            }
            break;
        } else {
            usage();
        }
        argc--;
        argv++;
    }
    if (kernel_fn == NULL) {
        usage();
    }
    if (!nodename) {
        nodename = getenv("MAGENTA_NODENAME");
    }
    if (nodename) {
        fprintf(stderr, "%s: Will only boot nodename '%s'\n", appname, nodename);
    }

    // compute the default ramdisk fn to use if
    // ramdisk is not specified and such a ramdisk
    // file actually exists
    char* auto_ramdisk_fn = NULL;
    if (ramdisk_fn == NULL) {
        char* bootdata_fn = "bootdata.bin";
        char *end = strrchr(kernel_fn, '/');
        if (end == NULL) {
            auto_ramdisk_fn = bootdata_fn;
        } else {
            size_t prefix_len = (end - kernel_fn) + 1;
            size_t len = prefix_len + strlen(bootdata_fn) + 1;
            if ((auto_ramdisk_fn = malloc(len)) != NULL) {
                memcpy(auto_ramdisk_fn, kernel_fn, prefix_len);
                memcpy(auto_ramdisk_fn + prefix_len, bootdata_fn, strlen(bootdata_fn) + 1);
            }
        }
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(NB_ADVERT_PORT);

    s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        fprintf(stderr, "%s: cannot create socket %d\n", appname, s);
        return -1;
    }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &n, sizeof(n));
    if ((r = bind(s, (void*)&addr, sizeof(addr))) < 0) {
        fprintf(stderr, "%s: cannot bind to [%s]%d %d: %s\n", appname,
                inet_ntop(AF_INET6, &addr.sin6_addr, tmp, sizeof(tmp)),
                ntohs(addr.sin6_port), errno, strerror(errno));
        return -1;
    }

    fprintf(stderr, "%s: listening on [%s]%d\n", appname,
            inet_ntop(AF_INET6, &addr.sin6_addr, tmp, sizeof(tmp)),
            ntohs(addr.sin6_port));

    for (;;) {
        struct sockaddr_in6 ra;
        socklen_t rlen;
        char buf[4096];
        nbmsg* msg = (void*)buf;
        rlen = sizeof(ra);
        r = recvfrom(s, buf, sizeof(buf) - 1, 0, (void*)&ra, &rlen);
        if (r < 0) {
            fprintf(stderr, "%s: socket read error %d\n", appname, r);
            break;
        }
        if (r < sizeof(nbmsg))
            continue;
        if (!IN6_IS_ADDR_LINKLOCAL(&ra.sin6_addr)) {
            fprintf(stderr, "%s: ignoring non-link-local message\n", appname);
            continue;
        }
        if (!IN6_IS_ADDR_UNSPECIFIED(&allowed_addr) && !IN6_ARE_ADDR_EQUAL(&allowed_addr, &ra.sin6_addr)) {
            fprintf(stderr, "%s: ignoring message not from allowed address '%s'\n", appname, inet_ntop(AF_INET6, &allowed_addr, tmp, sizeof(tmp)));
            continue;
        }
        if (msg->magic != NB_MAGIC)
            continue;
        if (msg->cmd != NB_ADVERTISE)
            continue;
        if (msg->arg != NB_VERSION_CURRENT) {
            fprintf(stderr, "%s: Incompatible version 0x%08X of bootloader detected from [%s]%d, please upgrade your bootloader\n", appname, msg->arg,
                    inet_ntop(AF_INET6, &ra.sin6_addr, tmp, sizeof(tmp)),
                    ntohs(ra.sin6_port));
            if (once) {
                break;
            }
            continue;
        }
        fprintf(stderr, "%s: got beacon from [%s]%d\n", appname,
                inet_ntop(AF_INET6, &ra.sin6_addr, tmp, sizeof(tmp)),
                ntohs(ra.sin6_port));

        // ensure any payload is null-terminated
        buf[r] = 0;


        char* save = NULL;
        char* adv_nodename = NULL;
        char* adv_version = "unknown";
        for (char* var = strtok_r((char*)msg->data, ";", &save); var; var = strtok_r(NULL, ";", &save)) {
            if (!strncmp(var, "nodename=", 9)) {
                adv_nodename = var + 9;
            } else if(!strncmp(var, "version=", 8)) {
                adv_version = var + 8;
            }
        }

        if (nodename) {
            if (adv_nodename == NULL) {
                fprintf(stderr, "%s: ignoring unknown nodename (expecting %s)\n",
                        appname, nodename);
            } else if (strcmp(adv_nodename, nodename)) {
                fprintf(stderr, "%s: ignoring nodename %s (expecting %s)\n",
                        appname, adv_nodename, nodename);
                continue;
            }
        }

        if (strcmp(BOOTLOADER_VERSION, adv_version)) {
            fprintf(stderr,
                    "%s: WARNING:\n"
                    "%s: WARNING: Bootloader version '%s' != '%s'. Please Upgrade.\n"
                    "%s: WARNING:\n",
                    appname, appname, adv_version, BOOTLOADER_VERSION, appname);
        }

        if (cmdline[0]) {
            status = xfer(&ra, "(cmdline)", cmdline, false);
        } else {
            status = 0;
        }
        if (status == 0) {
            struct stat s;
            if (ramdisk_fn) {
                status = xfer(&ra, ramdisk_fn, "ramdisk.bin", false);
            } else if (auto_ramdisk_fn && (stat(auto_ramdisk_fn, &s) == 0)) {
                status = xfer(&ra, auto_ramdisk_fn, "ramdisk.bin", false);
            }
        }
        if (status == 0) {
            xfer(&ra, kernel_fn, "kernel.bin", true);
        }
        if (once) {
            break;
        }
        drain(s);
    }

    return 0;
}
