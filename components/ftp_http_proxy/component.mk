# Configuration pour ESP-IDF
COMPONENT_ADD_INCLUDEDIRS := .
COMPONENT_PRIV_REQUIRED := esp_http_server lwip
COMPONENT_ADD_INCLUDEDIRS := .
COMPONENT_PRIV_REQUIRES := lwip esp_http_server esp_netif
