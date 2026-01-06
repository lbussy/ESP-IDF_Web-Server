idf_component_register(
    SRCS "http_server.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server
    PRIV_REQUIRES joltwallet__littlefs
)
