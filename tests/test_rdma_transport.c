#define _POSIX_C_SOURCE 199309L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <getopt.h>

#include "bitripple.h"
#include "bitripple_types.h"

#ifndef DEFAULT_IP
#define DEFAULT_IP "172.28.49.50"
#endif

#ifndef DEFAULT_PORT
#define DEFAULT_PORT 13337
#endif

#define SYMBOL_SIZE 64
#define DATA_SIZE_SMALL 25
#define DATA_SIZE_LARGE 2250

static volatile int running = 1;

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

static void print_usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -m <mode>   Mode: server or client (default: client)\n");
    printf("  -a <addr>   Server address (default: %s)\n", DEFAULT_IP);
    printf("  -p <port>   Port (default: %d)\n", DEFAULT_PORT);
    printf("  -n <num>    Number of paths (default: 2)\n");
    printf("  -s <size>   Data size: small (%d), large (%d), or custom\n",
           DATA_SIZE_SMALL, DATA_SIZE_LARGE);
    printf("  -v          Verbose output\n");
    printf("  -h          Show this help\n");
}

static uint64_t timestamp_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

int main(int argc, char* argv[]) {
    const char* mode = "client";
    const char* server_ip = DEFAULT_IP;
    uint16_t port = DEFAULT_PORT;
    uint32_t num_paths = 2;
    size_t data_size = DATA_SIZE_SMALL;
    int verbose = 0;

    int opt;
    while ((opt = getopt(argc, argv, "m:a:p:n:s:vh")) != -1) {
        switch (opt) {
            case 'm': mode = optarg; break;
            case 'a': server_ip = optarg; break;
            case 'p': port = (uint16_t)atoi(optarg); break;
            case 'n': num_paths = (uint32_t)atoi(optarg); break;
            case 's':
                if (strcmp(optarg, "small") == 0) {
                    data_size = DATA_SIZE_SMALL;
                } else if (strcmp(optarg, "large") == 0) {
                    data_size = DATA_SIZE_LARGE;
                } else {
                    data_size = (size_t)atoi(optarg);
                }
                break;
            case 'v': verbose = 1; break;
            case 'h': print_usage(argv[0]); return 0;
            default: print_usage(argv[0]); return 1;
        }
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int is_server = (strcmp(mode, "server") == 0);

    printf("=== BitRipple RDMA Transport Test ===\n");
    printf("Mode:      %s\n", mode);
    printf("Server:    %s:%d\n", server_ip, port);
    printf("Paths:     %u\n", num_paths);
    printf("Data size: %zu bytes\n", data_size);
    printf("\n");

    if (br_init() != 0) {
        fprintf(stderr, "Failed to initialize library\n");
        return 1;
    }

    if (is_server) {
        printf("[1/3] Creating server on port %d...\n", port);
        br_server_t* server = br_server_new(port, data_size, SYMBOL_SIZE,
                                             server_ip, port + 1);
        if (!server) {
            fprintf(stderr, "Failed to create server\n");
            br_cleanup();
            return 1;
        }

        printf("[2/3] Accepting %u paths...\n", num_paths);
        int ret = br_server_accept_paths(server, num_paths);
        if (ret != BR_OK) {
            fprintf(stderr, "Failed to accept paths: %d\n", ret);
            br_server_free(server);
            br_cleanup();
            return 1;
        }
        printf("    Accepted %u path(s)\n", num_paths);

        printf("[3/3] Receiving data (%zu bytes)...\n", data_size);
        uint8_t* out = (uint8_t*)malloc(data_size);
        if (!out) {
            fprintf(stderr, "Failed to allocate buffer\n");
            br_server_free(server);
            br_cleanup();
            return 1;
        }

        uint64_t recv_start = timestamp_us();
        ret = br_server_recv(server, out, data_size, 60000);
        uint64_t recv_end = timestamp_us();
        double elapsed_s = (double)(recv_end - recv_start) / 1e6;

        if (ret == BR_OK) {
            printf("    Received %zu bytes in %.2f s (%.1f KB/s)\n",
                   data_size, elapsed_s,
                   elapsed_s > 0 ? (double)data_size / elapsed_s / 1024 : 0);
            if (verbose) {
                printf("    Data: ");
                for (size_t i = 0; i < data_size && i < 64; i++) {
                    printf("%02x ", out[i]);
                }
                printf("\n");
            }
        } else {
            fprintf(stderr, "Receive failed: %d\n", ret);
        }

        free(out);
        br_server_free(server);

    } else {
        printf("[1/3] Creating profile and spray...\n");
        br_profile_t* prof = br_profile_new(num_paths, 1024);
        if (!prof) {
            fprintf(stderr, "Failed to create profile\n");
            br_cleanup();
            return 1;
        }
        br_profile_set_uniform(prof);

        const char* hosts[2] = { server_ip, server_ip };  // Fix: provide array of 2 host pointers
        const uint16_t ports[2] = { port, port };        // Fix: provide array of 2 port values

        printf("[2/3] Creating client connecting to %s:%d...\n", server_ip, port);
        br_client_t* client = br_client_new(prof, hosts, ports, num_paths,
                                             SYMBOL_SIZE, port + 1);
        if (!client) {
            fprintf(stderr, "Failed to create client\n");
            br_profile_free(prof);
            br_cleanup();
            return 1;
        }

        printf("[3/3] Generating and sending data (%zu bytes)...\n", data_size);
        uint8_t* data = (uint8_t*)malloc(data_size);
        if (!data) {
            fprintf(stderr, "Failed to allocate data\n");
            br_client_free(client);
            br_profile_free(prof);
            br_cleanup();
            return 1;
        }

        srand((unsigned int)time(NULL));
        for (size_t i = 0; i < data_size; i++) {
            data[i] = (uint8_t)(rand() & 0xFF);
        }

        if (verbose) {
            printf("    Data: ");
            for (size_t i = 0; i < data_size && i < 64; i++) {
                printf("%02x ", data[i]);
            }
            printf("\n");
        }

        uint64_t send_start = timestamp_us();
        int ret = br_client_send(client, data, data_size);
        uint64_t send_end = timestamp_us();
        double elapsed_s = (double)(send_end - send_start) / 1e6;

        if (ret == BR_OK) {
            printf("    Sent %zu bytes in %.2f s (%.1f KB/s)\n",
                   data_size, elapsed_s,
                   elapsed_s > 0 ? (double)data_size / elapsed_s / 1024 : 0);
        } else {
            fprintf(stderr, "Send failed: %d\n", ret);
        }

        br_client_poll_feedback(client, 5000);

        free(data);
        br_client_free(client);
        br_profile_free(prof);
    }

    br_cleanup();
    printf("\n=== Test Complete ===\n");
    return 0;
}