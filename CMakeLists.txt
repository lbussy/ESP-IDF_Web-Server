set(priv_requires "")

if(CONFIG_HTTP_SERVER_ENABLE_LITTLEFS)
    list(APPEND priv_requires joltwallet__littlefs)
endif()

if(priv_requires)
    idf_component_register(
        SRCS "src/http_server.cpp"
        INCLUDE_DIRS "include"
        REQUIRES esp_http_server
        PRIV_REQUIRES ${priv_requires}
    )
else()
    idf_component_register(
        SRCS "src/http_server.cpp"
        INCLUDE_DIRS "include"
        REQUIRES esp_http_server
    )
endif()
