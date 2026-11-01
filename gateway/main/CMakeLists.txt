idf_component_register(SRCS "app.c" "ble.c" "../../common/ble_wire.c" "../../common/ble_rpc.c" "updates.c" "../../common/update.c" "bridge.c" "display.c" "web.c" "wifi.c" "infrared.c"
    "../../common/lamp_control.c" "../../common/model.c" "../../common/wire.c" "../../common/infrared.c" "../../common/scenes.c"
    INCLUDE_DIRS "." "../../common"
    REQUIRES bt nvs_flash esp_wifi esp_event esp_netif esp_http_server esp_lcd driver esp_timer json app_update esp_http_client mbedtls
    EMBED_TXTFILES "web/index.html" "web/app.js" "web/style.css")
idf_component_get_property(json_lib json COMPONENT_LIB)
target_compile_definitions(${json_lib} PRIVATE CJSON_NESTING_LIMIT=16)

# Backport the async-upload fixes from IDF 5.3.4 (IDFGH-13053): the original
# HTTP task must neither discard the body nor read a socket owned by the worker.
# Generate a fixed source in the build directory; leave the installed SDK intact.
# https://github.com/espressif/esp-idf/issues/13998
idf_component_get_property(http_dir esp_http_server COMPONENT_DIR)
idf_component_get_property(http_lib esp_http_server COMPONENT_LIB)
file(READ "${http_dir}/src/httpd_txrx.c" http_source)
string(FIND "${http_source}" "r_aux->remaining_len = 0;" async_body_fixed)
if(async_body_fixed EQUAL -1)
    if(NOT "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}" VERSION_EQUAL "5.3.1")
        message(FATAL_ERROR "This IDF needs the async HTTP request-body fix (IDFGH-13053).")
    endif()
    set(marker "// mark socket as \"in use\"")
    string(FIND "${http_source}" "${marker}" marker_position)
    if(marker_position EQUAL -1)
        message(FATAL_ERROR "Unrecognized HTTP server source; cannot apply async body fix.")
    endif()
    string(REPLACE "${marker}" "r_aux->remaining_len = 0;\n\n    ${marker}" http_source "${http_source}")
    set(fixed_source "${CMAKE_CURRENT_BINARY_DIR}/httpd_txrx_fixed.c")
    file(WRITE "${fixed_source}" "${http_source}")
    get_target_property(http_sources ${http_lib} SOURCES)
    list(FIND http_sources "${http_dir}/src/httpd_txrx.c" source_index)
    if(source_index EQUAL -1)
        message(FATAL_ERROR "Cannot locate HTTP transport source for async body fix.")
    endif()
    list(REMOVE_AT http_sources ${source_index})
    list(APPEND http_sources "${fixed_source}")
    foreach(name IN ITEMS httpd_main httpd_sess)
        file(READ "${http_dir}/src/${name}.c" http_source)
        if(name STREQUAL "httpd_main")
            string(REPLACE "if (session->fd < 0)" "if (session->fd < 0 || session->for_async_req)" http_source "${http_source}")
        else()
            string(REPLACE "session->ctx = NULL;" "session->ctx = NULL;\n        session->for_async_req = false;" http_source "${http_source}")
            string(REPLACE "case HTTPD_TASK_SET_DESCRIPTOR:\n        if (session->fd != -1)"
                "case HTTPD_TASK_SET_DESCRIPTOR:\n        if (session->fd != -1 && !session->for_async_req)" http_source "${http_source}")
        endif()
        set(fixed_source "${CMAKE_CURRENT_BINARY_DIR}/${name}_fixed.c")
        file(WRITE "${fixed_source}" "${http_source}")
        list(REMOVE_ITEM http_sources "${http_dir}/src/${name}.c")
        list(APPEND http_sources "${fixed_source}")
    endforeach()
    set_property(TARGET ${http_lib} PROPERTY SOURCES "${http_sources}")
    target_include_directories(${http_lib} PRIVATE "${http_dir}/src")
endif()
