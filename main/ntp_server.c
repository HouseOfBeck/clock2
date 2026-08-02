#include "ntp_server.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lwip/def.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#include "ntp_path_diagnostics.h"
#include "ntp_time.h"
#include "timebase.h"

#define NTP_SERVER_PORT             123
#define NTP_PACKET_SIZE             48
#define NTP_REFERENCE_ID            0x47505300  /* "GPS\0" */
/* NTP short format (16.16): 66 / 65536 seconds is approximately 1 ms. */
#define NTP_ROOT_DISPERSION         66U
#define NTP_LOG_REPLY_INTERVAL      60

#ifndef NTP_SERVER_DEBUG
#define NTP_SERVER_DEBUG            0
#endif

/* 2^-20 seconds is about 0.954 us, matching esp_timer's 1 us resolution. */
#define NTP_PRECISION_EXPONENT      (-20)

#define NTP_LEAP_NO_WARNING         0
#define NTP_VERSION_3               3
#define NTP_VERSION_4               4
#define NTP_MODE_CLIENT             3
#define NTP_MODE_SERVER             4
#define NTP_STRATUM_PRIMARY         1

typedef struct __attribute__((packed)) {
    uint32_t seconds;
    uint32_t fraction;
} ntp_timestamp_t;

typedef struct __attribute__((packed)) {
    uint8_t leap_version_mode;
    uint8_t stratum;
    int8_t poll;
    int8_t precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t reference_id;
    ntp_timestamp_t reference_timestamp;
    ntp_timestamp_t origin_timestamp;
    ntp_timestamp_t receive_timestamp;
    ntp_timestamp_t transmit_timestamp;
} ntp_packet_t;

typedef struct {
    uint64_t received_packets;
    uint64_t transmitted_packets;
    uint64_t ignored_packets;
    uint64_t invalid_timebase_requests;
} ntp_server_stats_t;

_Static_assert(sizeof(ntp_packet_t) == NTP_PACKET_SIZE,
               "NTP packet must be exactly 48 bytes");

static const char *TAG = "clock2-ntp";

static struct udp_pcb *ntp_pcb;
static portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;
static ntp_server_stats_t stats;

#if NTP_PATH_DIAGNOSTICS
typedef struct {
    uint32_t pps_count;
    int64_t utc_second;
    int64_t pps_timestamp_us;
    int64_t now_us;
    int64_t delta_us;
    int64_t unix_us;
    uint32_t ntp_seconds;
    uint32_t ntp_fraction;
} ntp_timecheck_t;
#endif

static void stats_record_received(void)
{
    portENTER_CRITICAL(&stats_lock);
    stats.received_packets++;
    portEXIT_CRITICAL(&stats_lock);
}

static void stats_record_ignored(bool invalid_timebase)
{
    portENTER_CRITICAL(&stats_lock);
    stats.ignored_packets++;
    if (invalid_timebase) {
        stats.invalid_timebase_requests++;
    }
    portEXIT_CRITICAL(&stats_lock);
}

static uint64_t stats_record_transmitted(void)
{
    uint64_t transmitted_packets;

    portENTER_CRITICAL(&stats_lock);
    stats.transmitted_packets++;
    transmitted_packets = stats.transmitted_packets;
    portEXIT_CRITICAL(&stats_lock);

    return transmitted_packets;
}

void ntp_server_log_stats(void)
{
    ntp_server_stats_t snapshot;

    portENTER_CRITICAL(&stats_lock);
    snapshot = stats;
    portEXIT_CRITICAL(&stats_lock);

    ESP_LOGI(
        TAG,
        "NTP stats received=%" PRIu64 " transmitted=%" PRIu64
        " ignored=%" PRIu64 " invalid_timebase=%" PRIu64,
        snapshot.received_packets,
        snapshot.transmitted_packets,
        snapshot.ignored_packets,
        snapshot.invalid_timebase_requests);
}

static void ntp_value_to_network_timestamp(
    const ntp_time_value_t *value,
    ntp_timestamp_t *timestamp)
{
    timestamp->seconds = lwip_htonl(value->ntp_seconds);
    timestamp->fraction = lwip_htonl(value->ntp_fraction);
}

static bool get_current_ntp_time(
    ntp_timestamp_t *current_timestamp,
    ntp_timestamp_t *reference_timestamp,
    int64_t *sample_timestamp_us
#if NTP_PATH_DIAGNOSTICS
    , ntp_timecheck_t *timecheck
#endif
    )
{
    timebase_snapshot_t snapshot;
    if (!timebase_get_snapshot(&snapshot) || !snapshot.valid) {
        return false;
    }

    const int64_t now_us = esp_timer_get_time();
    ntp_time_value_t current_value;
    if (!ntp_time_from_pps(
            snapshot.unix_seconds,
            snapshot.pps_timestamp_us,
            now_us,
            &current_value)) {
        return false;
    }

    ntp_value_to_network_timestamp(&current_value, current_timestamp);

    if (reference_timestamp != NULL) {
        ntp_time_value_t reference_value;
        if (!ntp_time_from_unix(
                snapshot.unix_seconds, 0, &reference_value)) {
            return false;
        }
        ntp_value_to_network_timestamp(
            &reference_value,
            reference_timestamp);
    }

    if (sample_timestamp_us != NULL) {
        *sample_timestamp_us = now_us;
    }

#if NTP_PATH_DIAGNOSTICS
    if (timecheck != NULL) {
        *timecheck = (ntp_timecheck_t) {
            .pps_count = snapshot.pps_count,
            .utc_second = snapshot.unix_seconds,
            .pps_timestamp_us = snapshot.pps_timestamp_us,
            .now_us = now_us,
            .delta_us = now_us - snapshot.pps_timestamp_us,
            .unix_us = current_value.unix_seconds * 1000000LL +
                       current_value.microseconds,
            .ntp_seconds = current_value.ntp_seconds,
            .ntp_fraction = current_value.ntp_fraction,
        };
    }
#endif

    return true;
}

#if NTP_PATH_DIAGNOSTICS
static void log_timecheck(const char *kind, const ntp_timecheck_t *check)
{
    ESP_LOGI(
        TAG,
        "NTP timecheck kind=%s pps_count=%" PRIu32
        " utc_second=%" PRId64 " pps_timestamp_us=%" PRId64
        " now_us=%" PRId64 " delta_us=%" PRId64
        " unix_us=%" PRId64 " ntp_seconds=%" PRIu32
        " ntp_fraction=0x%08" PRIx32,
        kind,
        check->pps_count,
        check->utc_second,
        check->pps_timestamp_us,
        check->now_us,
        check->delta_us,
        check->unix_us,
        check->ntp_seconds,
        check->ntp_fraction);
}
#endif

static void ntp_receive_callback(
    void *arg,
    struct udp_pcb *pcb,
    struct pbuf *request_pbuf,
    const ip_addr_t *client_address,
    uint16_t client_port)
{
    (void)arg;

#if NTP_PATH_DIAGNOSTICS
    const int64_t callback_entry_us = esp_timer_get_time();
    ntp_path_rx_snapshot_t path_snapshot;
#endif

    if (request_pbuf == NULL) {
        return;
    }

#if NTP_PATH_DIAGNOSTICS
    ntp_path_diagnostics_capture_rx(
        callback_entry_us,
        &path_snapshot);
#endif

    stats_record_received();

    ntp_packet_t request = {0};
    const bool packet_size_valid =
        request_pbuf->tot_len >= NTP_PACKET_SIZE;
    const uint16_t copied = packet_size_valid
                                ? pbuf_copy_partial(
                                      request_pbuf,
                                      &request,
                                      sizeof(request),
                                      0)
                                : 0;
    pbuf_free(request_pbuf);

    const uint8_t client_version =
        (request.leap_version_mode >> 3) & 0x07U;
    if (copied != sizeof(request) ||
        (request.leap_version_mode & 0x07U) != NTP_MODE_CLIENT ||
        (client_version != NTP_VERSION_3 &&
         client_version != NTP_VERSION_4)) {
        stats_record_ignored(false);
        return;
    }

    ntp_packet_t reply = {
        .leap_version_mode =
            (NTP_LEAP_NO_WARNING << 6) |
            (NTP_VERSION_4 << 3) |
            NTP_MODE_SERVER,
        .stratum = NTP_STRATUM_PRIMARY,
        .poll = request.poll,
        .precision = NTP_PRECISION_EXPONENT,
        .root_delay = 0,
        .root_dispersion = lwip_htonl(NTP_ROOT_DISPERSION),
        .reference_id = lwip_htonl(NTP_REFERENCE_ID),
        .origin_timestamp = request.transmit_timestamp,
    };

    int64_t receive_timestamp_sample_us = 0;
#if NTP_PATH_DIAGNOSTICS
    ntp_timecheck_t receive_timecheck;
#endif
    if (!get_current_ntp_time(
            &reply.receive_timestamp,
            NULL,
            &receive_timestamp_sample_us
#if NTP_PATH_DIAGNOSTICS
            , &receive_timecheck
#endif
            )) {
        stats_record_ignored(true);
        return;
    }

    /*
     * Allocate one response pbuf per request. The lwIP transmit path prepends
     * protocol headers in the pbuf's reserved headroom and can leave its
     * len/tot_len changed, so a pbuf must not be reused for another reply.
     */
    struct pbuf *response_pbuf = pbuf_alloc(
        PBUF_TRANSPORT,
        NTP_PACKET_SIZE,
        PBUF_RAM);
    if (response_pbuf == NULL) {
        stats_record_ignored(false);
        return;
    }

    /* Generate transmit and reference timestamps immediately before send. */
#if NTP_PATH_DIAGNOSTICS
    int64_t transmit_timestamp_sample_us = 0;
    ntp_timecheck_t transmit_timecheck;
#endif
    if (!get_current_ntp_time(
            &reply.transmit_timestamp,
            &reply.reference_timestamp,
#if NTP_PATH_DIAGNOSTICS
            &transmit_timestamp_sample_us,
            &transmit_timecheck)) {
#else
            NULL)) {
#endif
        pbuf_free(response_pbuf);
        stats_record_ignored(true);
        return;
    }

    memcpy(response_pbuf->payload, &reply, sizeof(reply));

    if (response_pbuf->len != NTP_PACKET_SIZE ||
        response_pbuf->tot_len != NTP_PACKET_SIZE) {
        pbuf_free(response_pbuf);
        stats_record_ignored(false);
        return;
    }

#if NTP_PATH_DIAGNOSTICS
    const int64_t before_send_us = esp_timer_get_time();
#endif
    const err_t send_result = udp_sendto(
        pcb,
        response_pbuf,
        client_address,
        client_port);
#if NTP_PATH_DIAGNOSTICS
    const int64_t after_send_us = esp_timer_get_time();
    ntp_path_tx_snapshot_t tx_path_snapshot;
    ntp_path_diagnostics_capture_tx(
        before_send_us,
        after_send_us,
        &tx_path_snapshot);
#endif
    pbuf_free(response_pbuf);

#if NTP_PATH_DIAGNOSTICS
    log_timecheck("receive", &receive_timecheck);
    log_timecheck("transmit", &transmit_timecheck);
    ntp_path_diagnostics_log_request(
        &path_snapshot,
        callback_entry_us,
        receive_timestamp_sample_us,
        before_send_us,
        after_send_us);
    ntp_path_diagnostics_log_tx(
        &tx_path_snapshot,
        transmit_timestamp_sample_us,
        before_send_us,
        after_send_us);
#endif

    if (send_result != ERR_OK) {
        stats_record_ignored(false);
        return;
    }

    const uint64_t reply_count = stats_record_transmitted();

#if NTP_SERVER_DEBUG
    char client_ip[IPADDR_STRLEN_MAX];
    ipaddr_ntoa_r(client_address, client_ip, sizeof(client_ip));
    ESP_LOGI(
        TAG,
        "NTP client=%s receive=%" PRIu32 ".%08" PRIx32
        " transmit=%" PRIu32 ".%08" PRIx32,
        client_ip,
        lwip_ntohl(reply.receive_timestamp.seconds),
        lwip_ntohl(reply.receive_timestamp.fraction),
        lwip_ntohl(reply.transmit_timestamp.seconds),
        lwip_ntohl(reply.transmit_timestamp.fraction));
#endif

    if (reply_count % NTP_LOG_REPLY_INTERVAL == 0) {
        ESP_LOGI(TAG, "NTP replies=%" PRIu64, reply_count);
        ntp_server_log_stats();
    }
}

static esp_err_t ntp_server_init_in_tcpip(void *context)
{
    (void)context;

    ntp_pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (ntp_pcb == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const err_t bind_result =
        udp_bind(ntp_pcb, IP4_ADDR_ANY, NTP_SERVER_PORT);
    if (bind_result != ERR_OK) {
        udp_remove(ntp_pcb);
        ntp_pcb = NULL;
        return ESP_FAIL;
    }

    udp_recv(ntp_pcb, ntp_receive_callback, NULL);
    return ESP_OK;
}

void ntp_server_start(void)
{
#if NTP_PATH_DIAGNOSTICS
    ESP_ERROR_CHECK(ntp_time_run_self_tests() ? ESP_OK : ESP_FAIL);
    ESP_LOGI(TAG, "NTP time self-tests passed");
#endif

    ESP_ERROR_CHECK(esp_netif_tcpip_exec(
        ntp_server_init_in_tcpip,
        NULL));

    ESP_LOGI(TAG, "NTP server started on UDP port %d", NTP_SERVER_PORT);
}
