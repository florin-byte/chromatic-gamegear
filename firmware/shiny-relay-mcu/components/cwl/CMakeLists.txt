cmake_minimum_required(VERSION 3.22)

idf_component_register(SRCS "cwl_esp.c" "cwl_proto.c" "cwl_relay.c" "cwl_online.c" "cwl_lx.c" "cwl_espnow.c" "cwl_net.c" "cwl_sec.c" "cwl_wdt.c" "cwl_portal.c" "cwl_tls.c" "cwl_share.c" "cwl_time.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi esp_netif esp_event nvs_flash console esp_timer
                             esp_driver_uart lwip crc button esp_http_client mbedtls json)
