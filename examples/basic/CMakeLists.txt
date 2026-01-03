cmake_minimum_required(VERSION 3.16)

# If you want the example to use the component from the local repo
# export HTTP_SERVER_LOCAL_DEV=1
# And comment the version the example/main/idf_component.yml
if(
    DEFINED
        ENV{HTTP_SERVER_LOCAL_DEV} AND "$ENV{HTTP_SERVER_LOCAL_DEV}" STREQUAL "1"
    )
    set(
        EXTRA_COMPONENT_DIRS
            ${CMAKE_CURRENT_LIST_DIR}/../../components/http_server
    )
endif()

include(
    $ENV{IDF_PATH}/tools/cmake/project.cmake
)

project(
    http_server_basic_example
)
