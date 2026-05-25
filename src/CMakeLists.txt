file(
    GLOB_RECURSE app_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "." "../include"
    REQUIRES app_update driver esp_event esp_http_client esp_http_server esp_https_ota esp_netif esp_wifi esp_adc lwip mdns nvs_flash led_strip spiffs json mbedtls
)
