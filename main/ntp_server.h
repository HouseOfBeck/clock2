#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool running;
    uint16_t port;
    uint8_t stratum;
    int8_t precision;
    char reference_id[4];
    uint64_t received_packets;
    uint64_t transmitted_packets;
    uint64_t ignored_packets;
    uint64_t invalid_timebase_requests;
} ntp_server_snapshot_t;

void ntp_server_start(void);
void ntp_server_log_stats(void);
void ntp_server_get_snapshot(ntp_server_snapshot_t *snapshot);
