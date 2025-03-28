# Required components for ESP-IDF
set(COMPONENT_REQUIRES 
    esp_http_server
    esp_netif
    esp_timer
    lwip
)

# Add component requirements
set(COMPONENT_PRIV_REQUIRES 
    esp_http_server
)
