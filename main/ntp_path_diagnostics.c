#include "ntp_path_diagnostics.h"

#include <inttypes.h>
#include <stddef.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "wiznet_spi.h"

#define W5500_SOCKET0_IR_OFFSET        0x0002U
#define W5500_SOCKET0_REGISTER_BSB     0x01U
#define W5500_SOCKET0_RX_BUFFER_BSB    0x03U
#define W5500_SOCKET_IR_RECV           (1U << 2)

#define ETHERNET_HEADER_SIZE           14U
#define ETHERNET_VLAN_HEADER_SIZE      18U
#define ETHERNET_TYPE_IPV4             0x0800U
#define ETHERNET_TYPE_VLAN             0x8100U
#define IPV4_PROTOCOL_UDP              17U
#define NTP_UDP_PORT                   123U

#define NTP_PATH_FRAME_HISTORY_SIZE    4U
#define NTP_PATH_SUMMARY_INTERVAL      20U

typedef struct {
    int64_t irq_timestamp_us;
    int64_t rx_processing_start_us;
    int64_t spi_frame_read_done_us;
    uint32_t irq_sequence;
    uint32_t frame_sequence;
    uint8_t socket_interrupt_status;
} ntp_path_frame_sample_t;

typedef struct {
    uint64_t count;
    uint64_t irq_to_callback_sum_us;
    uint64_t callback_to_send_sum_us;
    int64_t irq_to_callback_min_us;
    int64_t irq_to_callback_max_us;
    int64_t callback_to_send_min_us;
    int64_t callback_to_send_max_us;
} ntp_path_stats_t;

typedef struct {
    gpio_isr_t handler;
    void *argument;
} original_gpio_isr_t;

#if NTP_PATH_DIAGNOSTICS
static const char *TAG = "clock2-ntp-path";
static portMUX_TYPE path_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile int64_t latest_irq_timestamp_us;
static volatile uint32_t latest_irq_sequence;
static int diagnostic_gpio = -1;
static original_gpio_isr_t original_w5500_isr;
static int64_t active_rx_irq_timestamp_us;
static int64_t active_rx_processing_start_us;
static uint32_t active_rx_irq_sequence;
static uint8_t active_socket_interrupt_status;
static uint32_t next_frame_sequence;
static uint32_t last_consumed_frame_sequence;
static uint32_t previous_request_irq_sequence;
static ntp_path_frame_sample_t frame_history[NTP_PATH_FRAME_HISTORY_SIZE];
static ntp_path_stats_t path_stats;
#endif

esp_err_t __real_gpio_isr_handler_add(
    gpio_num_t gpio_num,
    gpio_isr_t isr_handler,
    void *args);

#if NTP_PATH_DIAGNOSTICS
static void IRAM_ATTR w5500_diagnostic_isr(void *argument)
{
    (void)argument;

    const int64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL_ISR(&path_lock);
    latest_irq_timestamp_us = now_us;
    latest_irq_sequence++;
    portEXIT_CRITICAL_ISR(&path_lock);

    original_w5500_isr.handler(original_w5500_isr.argument);
}
#endif

esp_err_t __wrap_gpio_isr_handler_add(
    gpio_num_t gpio_num,
    gpio_isr_t isr_handler,
    void *args)
{
#if NTP_PATH_DIAGNOSTICS
    if ((int)gpio_num == diagnostic_gpio) {
        original_w5500_isr.handler = isr_handler;
        original_w5500_isr.argument = args;
        return __real_gpio_isr_handler_add(
            gpio_num,
            w5500_diagnostic_isr,
            NULL);
    }
#endif

    return __real_gpio_isr_handler_add(gpio_num, isr_handler, args);
}

#if NTP_PATH_DIAGNOSTICS
static uint16_t read_u16_be(const uint8_t *bytes)
{
    return ((uint16_t)bytes[0] << 8) | bytes[1];
}

static bool is_ntp_ipv4_udp_frame(const uint8_t *frame, uint32_t length)
{
    if (length < ETHERNET_HEADER_SIZE) {
        return false;
    }

    uint32_t ip_offset = ETHERNET_HEADER_SIZE;
    uint16_t ether_type = read_u16_be(&frame[12]);

    if (ether_type == ETHERNET_TYPE_VLAN) {
        if (length < ETHERNET_VLAN_HEADER_SIZE) {
            return false;
        }
        ether_type = read_u16_be(&frame[16]);
        ip_offset = ETHERNET_VLAN_HEADER_SIZE;
    }

    if (ether_type != ETHERNET_TYPE_IPV4 || length < ip_offset + 20U) {
        return false;
    }

    const uint8_t version_and_ihl = frame[ip_offset];
    const uint32_t ip_header_length =
        (uint32_t)(version_and_ihl & 0x0FU) * 4U;
    if ((version_and_ihl >> 4) != 4U || ip_header_length < 20U ||
        length < ip_offset + ip_header_length + 8U ||
        frame[ip_offset + 9U] != IPV4_PROTOCOL_UDP) {
        return false;
    }

    const uint16_t fragment_offset =
        read_u16_be(&frame[ip_offset + 6U]) & 0x1FFFU;
    if (fragment_offset != 0U) {
        return false;
    }

    const uint32_t udp_offset = ip_offset + ip_header_length;
    return read_u16_be(&frame[udp_offset + 2U]) == NTP_UDP_PORT;
}

static bool is_socket0_interrupt_read(uint32_t cmd, uint32_t addr, uint32_t length)
{
    const uint32_t block_select =
        (addr >> WIZNET_BSB_OFFSET) & 0x1FU;
    return cmd == W5500_SOCKET0_IR_OFFSET &&
           block_select == W5500_SOCKET0_REGISTER_BSB &&
           length == 1U;
}

static bool is_socket0_rx_buffer_read(uint32_t addr, uint32_t length)
{
    const uint32_t block_select =
        (addr >> WIZNET_BSB_OFFSET) & 0x1FU;
    return block_select == W5500_SOCKET0_RX_BUFFER_BSB && length > 2U;
}

static void *diagnostic_spi_init(const void *config)
{
    return wiznet_spi_init(config);
}

static esp_err_t diagnostic_spi_deinit(void *context)
{
    return wiznet_spi_deinit(context);
}

static esp_err_t diagnostic_spi_write(
    void *context,
    uint32_t cmd,
    uint32_t addr,
    const void *data,
    uint32_t length)
{
    return wiznet_spi_write(context, cmd, addr, data, length);
}

static esp_err_t diagnostic_spi_read(
    void *context,
    uint32_t cmd,
    uint32_t addr,
    void *data,
    uint32_t length)
{
    const bool interrupt_read =
        is_socket0_interrupt_read(cmd, addr, length);
    const int64_t processing_start_us =
        interrupt_read ? esp_timer_get_time() : 0;

    const esp_err_t result =
        wiznet_spi_read(context, cmd, addr, data, length);
    if (result != ESP_OK) {
        return result;
    }

    if (interrupt_read &&
        ((const uint8_t *)data)[0] & W5500_SOCKET_IR_RECV) {
        portENTER_CRITICAL(&path_lock);
        active_rx_irq_timestamp_us = latest_irq_timestamp_us;
        active_rx_processing_start_us = processing_start_us;
        active_rx_irq_sequence = latest_irq_sequence;
        active_socket_interrupt_status = ((const uint8_t *)data)[0];
        portEXIT_CRITICAL(&path_lock);
    }

    if (is_socket0_rx_buffer_read(addr, length) &&
        is_ntp_ipv4_udp_frame((const uint8_t *)data, length)) {
        const int64_t read_done_us = esp_timer_get_time();

        portENTER_CRITICAL(&path_lock);
        next_frame_sequence++;
        ntp_path_frame_sample_t *sample =
            &frame_history[next_frame_sequence % NTP_PATH_FRAME_HISTORY_SIZE];
        sample->irq_timestamp_us = active_rx_irq_timestamp_us;
        sample->rx_processing_start_us = active_rx_processing_start_us;
        sample->spi_frame_read_done_us = read_done_us;
        sample->irq_sequence = active_rx_irq_sequence;
        sample->frame_sequence = next_frame_sequence;
        sample->socket_interrupt_status = active_socket_interrupt_status;
        portEXIT_CRITICAL(&path_lock);
    }

    return result;
}
#endif

void ntp_path_diagnostics_configure_w5500(
    eth_w5500_config_t *w5500_config)
{
#if NTP_PATH_DIAGNOSTICS
    diagnostic_gpio = w5500_config->base.int_gpio_num;
    w5500_config->base.custom_spi_driver.config = &w5500_config->base;
    w5500_config->base.custom_spi_driver.init = diagnostic_spi_init;
    w5500_config->base.custom_spi_driver.deinit = diagnostic_spi_deinit;
    w5500_config->base.custom_spi_driver.read = diagnostic_spi_read;
    w5500_config->base.custom_spi_driver.write = diagnostic_spi_write;
#else
    (void)w5500_config;
#endif
}

void ntp_path_diagnostics_capture_rx(
    int64_t callback_entry_us,
    ntp_path_rx_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));

#if NTP_PATH_DIAGNOSTICS
    ntp_path_frame_sample_t selected = {0};

    portENTER_CRITICAL(&path_lock);
    for (uint32_t index = 0; index < NTP_PATH_FRAME_HISTORY_SIZE; index++) {
        const ntp_path_frame_sample_t *candidate = &frame_history[index];
        if (candidate->frame_sequence > last_consumed_frame_sequence &&
            (selected.frame_sequence == 0U ||
             candidate->frame_sequence < selected.frame_sequence) &&
            candidate->spi_frame_read_done_us <= callback_entry_us) {
            selected = *candidate;
        }
    }

    if (selected.frame_sequence != 0U) {
        last_consumed_frame_sequence = selected.frame_sequence;
        snapshot->irq_edges_since_previous_request =
            selected.irq_sequence - previous_request_irq_sequence;
        previous_request_irq_sequence = selected.irq_sequence;
    }
    portEXIT_CRITICAL(&path_lock);

    snapshot->irq_timestamp_us = selected.irq_timestamp_us;
    snapshot->rx_processing_start_us = selected.rx_processing_start_us;
    snapshot->spi_frame_read_done_us = selected.spi_frame_read_done_us;
    snapshot->irq_sequence = selected.irq_sequence;
    snapshot->socket_interrupt_status =
        selected.socket_interrupt_status;

    const int64_t irq_age_us =
        callback_entry_us - selected.irq_timestamp_us;
    snapshot->irq_valid = selected.frame_sequence != 0U &&
                          selected.irq_timestamp_us > 0 &&
                          selected.rx_processing_start_us >=
                              selected.irq_timestamp_us &&
                          selected.spi_frame_read_done_us >=
                              selected.rx_processing_start_us &&
                          irq_age_us >= 0 &&
                          irq_age_us <= NTP_PATH_IRQ_STALE_WINDOW_US;
#else
    (void)callback_entry_us;
#endif
}

#if NTP_PATH_DIAGNOSTICS
static void update_min_max(
    int64_t value,
    int64_t *minimum,
    int64_t *maximum,
    uint64_t previous_count)
{
    if (previous_count == 0U || value < *minimum) {
        *minimum = value;
    }
    if (previous_count == 0U || value > *maximum) {
        *maximum = value;
    }
}
#endif

void ntp_path_diagnostics_log_request(
    const ntp_path_rx_snapshot_t *snapshot,
    int64_t callback_entry_us,
    int64_t receive_timestamp_sample_us,
    int64_t before_send_us,
    int64_t after_send_us)
{
#if NTP_PATH_DIAGNOSTICS
    const int64_t irq_to_callback_us = snapshot->irq_timestamp_us > 0
        ? callback_entry_us - snapshot->irq_timestamp_us
        : -1;
    const int64_t irq_to_rx_start_us =
        snapshot->irq_timestamp_us > 0 &&
        snapshot->rx_processing_start_us > 0
            ? snapshot->rx_processing_start_us -
                  snapshot->irq_timestamp_us
            : -1;
    const int64_t rx_start_to_spi_done_us =
        snapshot->rx_processing_start_us > 0 &&
        snapshot->spi_frame_read_done_us > 0
            ? snapshot->spi_frame_read_done_us -
                  snapshot->rx_processing_start_us
            : -1;
    const int64_t spi_done_to_callback_us =
        snapshot->spi_frame_read_done_us > 0
            ? callback_entry_us - snapshot->spi_frame_read_done_us
            : -1;
    const int64_t callback_to_rxstamp_us =
        receive_timestamp_sample_us - callback_entry_us;
    const int64_t rxstamp_to_send_us =
        before_send_us - receive_timestamp_sample_us;
    const int64_t send_call_us = after_send_us - before_send_us;
    const int64_t callback_to_send_us =
        before_send_us - callback_entry_us;

    ESP_LOGI(
        TAG,
        "NTP path irq_valid=%s irq_to_cb=%" PRId64
        " us irq_to_rx_start=%" PRId64
        " us rx_start_to_spi_done=%" PRId64
        " us spi_done_to_cb=%" PRId64
        " us cb_to_rxstamp=%" PRId64
        " us rxstamp_to_send=%" PRId64
        " us send_call=%" PRId64
        " us irq_seq=%" PRIu32 " irq_edges=%" PRIu32 " ir=0x%02x",
        snapshot->irq_valid ? "yes" : "no",
        irq_to_callback_us,
        irq_to_rx_start_us,
        rx_start_to_spi_done_us,
        spi_done_to_callback_us,
        callback_to_rxstamp_us,
        rxstamp_to_send_us,
        send_call_us,
        snapshot->irq_sequence,
        snapshot->irq_edges_since_previous_request,
        (unsigned)snapshot->socket_interrupt_status);

    if (!snapshot->irq_valid) {
        return;
    }

    const uint64_t previous_count = path_stats.count;
    update_min_max(
        irq_to_callback_us,
        &path_stats.irq_to_callback_min_us,
        &path_stats.irq_to_callback_max_us,
        previous_count);
    update_min_max(
        callback_to_send_us,
        &path_stats.callback_to_send_min_us,
        &path_stats.callback_to_send_max_us,
        previous_count);

    path_stats.count++;
    path_stats.irq_to_callback_sum_us +=
        (uint64_t)irq_to_callback_us;
    path_stats.callback_to_send_sum_us +=
        (uint64_t)callback_to_send_us;

    if (path_stats.count % NTP_PATH_SUMMARY_INTERVAL == 0U) {
        ESP_LOGI(
            TAG,
            "NTP path summary n=%" PRIu64
            " irq_to_cb_mean=%" PRIu64
            " us min=%" PRId64 " us max=%" PRId64
            " us cb_to_send_mean=%" PRIu64
            " us min=%" PRId64 " us max=%" PRId64 " us",
            path_stats.count,
            path_stats.irq_to_callback_sum_us / path_stats.count,
            path_stats.irq_to_callback_min_us,
            path_stats.irq_to_callback_max_us,
            path_stats.callback_to_send_sum_us / path_stats.count,
            path_stats.callback_to_send_min_us,
            path_stats.callback_to_send_max_us);
    }
#else
    (void)snapshot;
    (void)callback_entry_us;
    (void)receive_timestamp_sample_us;
    (void)before_send_us;
    (void)after_send_us;
#endif
}
