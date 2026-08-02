#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_eth_mac_w5500.h"

/* Set to 0 to remove per-packet path instrumentation and logging. */
#ifndef NTP_PATH_DIAGNOSTICS
#define NTP_PATH_DIAGNOSTICS 0
#endif

#define NTP_PATH_IRQ_STALE_WINDOW_US 20000

typedef struct {
    bool irq_valid;
    int64_t irq_timestamp_us;
    int64_t rx_processing_start_us;
    int64_t spi_frame_read_done_us;
    uint32_t irq_sequence;
    uint32_t irq_edges_since_previous_request;
    uint8_t socket_interrupt_status;
} ntp_path_rx_snapshot_t;

typedef struct {
    bool valid;
    int64_t spi_write_start_us;
    int64_t spi_write_done_us;
    int64_t send_command_us;
    int64_t send_ok_us;
    uint32_t sequence;
    uint32_t ethernet_frame_length;
} ntp_path_tx_snapshot_t;

void ntp_path_diagnostics_configure_w5500(
    eth_w5500_config_t *w5500_config);

void ntp_path_diagnostics_capture_rx(
    int64_t callback_entry_us,
    ntp_path_rx_snapshot_t *snapshot);

void ntp_path_diagnostics_log_request(
    const ntp_path_rx_snapshot_t *snapshot,
    int64_t callback_entry_us,
    int64_t receive_timestamp_sample_us,
    int64_t before_send_us,
    int64_t after_send_us);

void ntp_path_diagnostics_capture_tx(
    int64_t udp_send_entry_us,
    int64_t udp_send_return_us,
    ntp_path_tx_snapshot_t *snapshot);

void ntp_path_diagnostics_log_tx(
    const ntp_path_tx_snapshot_t *snapshot,
    int64_t transmit_timestamp_sample_us,
    int64_t udp_send_entry_us,
    int64_t udp_send_return_us);
