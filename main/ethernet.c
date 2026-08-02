#include "ethernet.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "freertos/FreeRTOS.h"

#include "ntp_path_diagnostics.h"

#define ETH_SPI_HOST       SPI2_HOST
#define ETH_MOSI_GPIO      11
#define ETH_MISO_GPIO      12
#define ETH_SCLK_GPIO      13
#define ETH_CS_GPIO        14
#define ETH_INT_GPIO       10
#define ETH_RESET_GPIO     9

#define ETH_SPI_CLOCK_HZ   (20 * 1000 * 1000)

static const char *TAG = "clock2-eth";

static esp_eth_handle_t eth_handle;
static esp_netif_t *eth_netif;
static esp_eth_netif_glue_handle_t eth_glue;
static portMUX_TYPE ethernet_lock = portMUX_INITIALIZER_UNLOCKED;
static ethernet_snapshot_t ethernet_state = {
    .mosi_gpio = ETH_MOSI_GPIO,
    .miso_gpio = ETH_MISO_GPIO,
    .sclk_gpio = ETH_SCLK_GPIO,
    .cs_gpio = ETH_CS_GPIO,
    .int_gpio = ETH_INT_GPIO,
    .reset_gpio = ETH_RESET_GPIO,
    .spi_clock_hz = ETH_SPI_CLOCK_HZ,
};

static void ethernet_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)arg;
    (void)event_base;

    esp_eth_handle_t handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_START:
        portENTER_CRITICAL(&ethernet_lock);
        ethernet_state.running = true;
        portEXIT_CRITICAL(&ethernet_lock);
        ESP_LOGI(TAG, "Ethernet started");
        break;

    case ETHERNET_EVENT_CONNECTED: {
        uint8_t mac[6] = {0};

        ESP_ERROR_CHECK(
            esp_eth_ioctl(handle, ETH_CMD_G_MAC_ADDR, mac));

        portENTER_CRITICAL(&ethernet_lock);
        ethernet_state.link_up = true;
        memcpy(ethernet_state.mac, mac, sizeof(mac));
        portEXIT_CRITICAL(&ethernet_lock);

        ESP_LOGI(TAG, "Ethernet link up");
        ESP_LOGI(
            TAG,
            "MAC %02X:%02X:%02X:%02X:%02X:%02X",
            mac[0], mac[1], mac[2],
            mac[3], mac[4], mac[5]);
        break;
    }

    case ETHERNET_EVENT_DISCONNECTED:
        portENTER_CRITICAL(&ethernet_lock);
        ethernet_state.link_up = false;
        ethernet_state.has_ipv4 = false;
        ethernet_state.ipv4 = 0;
        ethernet_state.netmask = 0;
        ethernet_state.gateway = 0;
        portEXIT_CRITICAL(&ethernet_lock);
        ESP_LOGW(TAG, "Ethernet link down");
        break;

    case ETHERNET_EVENT_STOP:
        portENTER_CRITICAL(&ethernet_lock);
        ethernet_state.running = false;
        ethernet_state.link_up = false;
        ethernet_state.has_ipv4 = false;
        portEXIT_CRITICAL(&ethernet_lock);
        ESP_LOGI(TAG, "Ethernet stopped");
        break;

    default:
        break;
    }
}

static void ethernet_got_ip_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;

    const ip_event_got_ip_t *event =
        (const ip_event_got_ip_t *)event_data;

    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac));

    portENTER_CRITICAL(&ethernet_lock);
    ethernet_state.has_ipv4 = true;
    ethernet_state.ipv4 = ip_info->ip.addr;
    ethernet_state.netmask = ip_info->netmask.addr;
    ethernet_state.gateway = ip_info->gw.addr;
    memcpy(ethernet_state.mac, mac, sizeof(mac));
    portEXIT_CRITICAL(&ethernet_lock);

    ESP_LOGI(TAG, "DHCP address acquired");
    ESP_LOGI(
        TAG,
        "IP " IPSTR "    MAC %02X:%02X:%02X:%02X:%02X:%02X",
        IP2STR(&ip_info->ip),
        mac[0], mac[1], mac[2],
        mac[3], mac[4], mac[5]);

    ESP_LOGI(TAG, "Netmask " IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "Gateway " IPSTR, IP2STR(&ip_info->gw));
}

esp_err_t ethernet_start(void)
{
    esp_err_t ret;

    /*
     * Initialize the TCP/IP stack and default event loop.
     * This test application creates each only once.
     */
    ESP_RETURN_ON_ERROR(
        esp_netif_init(),
        TAG,
        "esp_netif_init failed");

    ESP_RETURN_ON_ERROR(
        esp_event_loop_create_default(),
        TAG,
        "event loop creation failed");

    /*
     * SPI bus used by the onboard W5500.
     */
    const spi_bus_config_t bus_config = {
        .mosi_io_num = ETH_MOSI_GPIO,
        .miso_io_num = ETH_MISO_GPIO,
        .sclk_io_num = ETH_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(
            ETH_SPI_HOST,
            &bus_config,
            SPI_DMA_CH_AUTO),
        TAG,
        "SPI bus initialization failed");

    /*
     * W5500 SPI framing:
     *   16 command bits
     *    8 address bits
     */
    spi_device_interface_config_t spi_device_config = {
        .command_bits = 16,
        .address_bits = 8,
        .mode = 0,
        .clock_speed_hz = ETH_SPI_CLOCK_HZ,
        .spics_io_num = ETH_CS_GPIO,
        .queue_size = 20,
    };

    eth_w5500_config_t w5500_config =
        ETH_W5500_DEFAULT_CONFIG(
            ETH_SPI_HOST,
            &spi_device_config);

    w5500_config.base.int_gpio_num = ETH_INT_GPIO;
    ntp_path_diagnostics_configure_w5500(&w5500_config);

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();

    esp_eth_mac_t *mac =
        esp_eth_mac_new_w5500(
            &w5500_config,
            &mac_config);

    ESP_RETURN_ON_FALSE(
        mac != NULL,
        ESP_FAIL,
        TAG,
        "Could not create W5500 MAC");

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();

    /*
     * W5500 uses PHY address 1 in Espressif's SPI-Ethernet
     * driver. The Waveshare board exposes reset on GPIO9.
     */
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = ETH_RESET_GPIO;

    esp_eth_phy_t *phy =
        esp_eth_phy_new_w5500(&phy_config);

    if (phy == NULL) {
        mac->del(mac);
        ESP_LOGE(TAG, "Could not create W5500 PHY");
        return ESP_FAIL;
    }

    esp_eth_config_t eth_config =
        ETH_DEFAULT_CONFIG(mac, phy);

    ret = esp_eth_driver_install(
        &eth_config,
        &eth_handle);

    if (ret != ESP_OK) {
        phy->del(phy);
        mac->del(mac);
        ESP_LOGE(
            TAG,
            "Ethernet driver installation failed: %s",
            esp_err_to_name(ret));
        return ret;
    }

    /*
     * Assign the ESP32's factory-derived Ethernet MAC address
     * to the external W5500.
     */
    uint8_t mac_address[6];

    ESP_RETURN_ON_ERROR(
        esp_read_mac(mac_address, ESP_MAC_ETH),
        TAG,
        "Could not obtain Ethernet MAC address");

    portENTER_CRITICAL(&ethernet_lock);
    memcpy(ethernet_state.mac, mac_address, sizeof(mac_address));
    portEXIT_CRITICAL(&ethernet_lock);

    ESP_RETURN_ON_ERROR(
        esp_eth_ioctl(
            eth_handle,
            ETH_CMD_S_MAC_ADDR,
            mac_address),
        TAG,
        "Could not set Ethernet MAC address");

    /*
     * Create and attach the lwIP Ethernet interface.
     * DHCP starts automatically when Ethernet starts.
     */
    esp_netif_config_t netif_config =
        ESP_NETIF_DEFAULT_ETH();

    eth_netif = esp_netif_new(&netif_config);

    ESP_RETURN_ON_FALSE(
        eth_netif != NULL,
        ESP_FAIL,
        TAG,
        "Could not create Ethernet netif");

    eth_glue = esp_eth_new_netif_glue(eth_handle);

    ESP_RETURN_ON_FALSE(
        eth_glue != NULL,
        ESP_FAIL,
        TAG,
        "Could not create Ethernet netif glue");

    ESP_RETURN_ON_ERROR(
        esp_netif_attach(eth_netif, eth_glue),
        TAG,
        "Could not attach Ethernet netif");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(
            ETH_EVENT,
            ESP_EVENT_ANY_ID,
            ethernet_event_handler,
            NULL),
        TAG,
        "Could not register Ethernet event handler");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_ETH_GOT_IP,
            ethernet_got_ip_handler,
            NULL),
        TAG,
        "Could not register IP event handler");

    ESP_LOGI(
        TAG,
        "Starting W5500: MOSI=%d MISO=%d CLK=%d CS=%d INT=%d RST=%d",
        ETH_MOSI_GPIO,
        ETH_MISO_GPIO,
        ETH_SCLK_GPIO,
        ETH_CS_GPIO,
        ETH_INT_GPIO,
        ETH_RESET_GPIO);

    ESP_RETURN_ON_ERROR(
        esp_eth_start(eth_handle),
        TAG,
        "Could not start Ethernet");

    return ESP_OK;
}

bool ethernet_get_snapshot(ethernet_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return false;
    }

    portENTER_CRITICAL(&ethernet_lock);
    *snapshot = ethernet_state;
    portEXIT_CRITICAL(&ethernet_lock);
    return true;
}
