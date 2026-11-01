idf_component_register(SRCS "lamp.c" "../common/update.c" "../common/lamp_control.c" "../common/model.c" "../common/wire.c"
    INCLUDE_DIRS "." "../common" REQUIRES esp-zigbee-lib nvs_flash driver esp_timer json app_update mbedtls)
idf_component_get_property(json_lib json COMPONENT_LIB)
target_compile_definitions(${json_lib} PRIVATE CJSON_NESTING_LIMIT=16)
