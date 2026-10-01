set(MOTO_REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(MOTO_SHARED_UI_SOURCE
    "${MOTO_REPOSITORY_ROOT}/shared/nav_ui/src/moto_nav_ui.cpp")
set(MOTO_SHARED_UI_FONT
    "${MOTO_REPOSITORY_ROOT}/shared/nav_ui/assets/moto_font_nav_16.c")
set(MOTO_SHARED_UI_INCLUDE
    "${MOTO_REPOSITORY_ROOT}/shared/nav_ui/include")
set(MOTO_SHARED_NAV_PRESENTER_SOURCE
    "${MOTO_REPOSITORY_ROOT}/shared/nav_presenter/src/moto_nav_presenter.cpp")
set(MOTO_SHARED_BLE_PROTOCOL_SOURCE
    "${MOTO_REPOSITORY_ROOT}/shared/ble_protocol/src/ble_protocol.cpp")

if(CONFIG_MOTO_BOARD_WAVESHARE_1_75C)
    set(MOTO_BOARD_PORT_SOURCE "board_port_waveshare_1_75c.cpp")
    set(MOTO_BOARD_CANVAS_WIDTH 466)
    set(MOTO_BOARD_CANVAS_HEIGHT 466)
else()
    # The ESP-IDF requirements pass runs before Kconfig values are available.
    # Use the project's default board until sdkconfig is loaded on the second
    # pass, where the 1.75C choice above can select its own source and canvas.
    set(MOTO_BOARD_PORT_SOURCE "board_port_waveshare_1_85b.cpp")
    set(MOTO_BOARD_CANVAS_WIDTH 360)
    set(MOTO_BOARD_CANVAS_HEIGHT 360)
endif()

# The shared UI source is compiled directly into this ESP-IDF component. Do not
# copy it into platforms/esp32: Web and firmware must execute the same code.
idf_component_register(
    SRCS
        "app_main.cpp"
        "ble_nav_transport_nimble.cpp"
        "${MOTO_BOARD_PORT_SOURCE}"
        "motion_heading_sensor.cpp"
        "phone_nav_bridge.cpp"
        "${MOTO_SHARED_BLE_PROTOCOL_SOURCE}"
        "${MOTO_SHARED_NAV_PRESENTER_SOURCE}"
        "${MOTO_SHARED_UI_FONT}"
        "${MOTO_SHARED_UI_SOURCE}"
    INCLUDE_DIRS
        "."
        "${MOTO_REPOSITORY_ROOT}/shared"
        "${MOTO_REPOSITORY_ROOT}/shared/ble_protocol/include"
        "${MOTO_REPOSITORY_ROOT}/shared/nav_presenter/include"
        "${MOTO_SHARED_UI_INCLUDE}"
    REQUIRES
        esp_timer
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_spi
        esp_hw_support
        freertos
        log
        lvgl
        nvs_flash
        bt
        esp_driver_ledc
        esp_lcd
        esp_lcd_co5300
        esp_lcd_st77916
        esp_lcd_touch
        esp_lcd_touch_cst816s
        esp_lvgl_adapter
        esp32_s3_touch_amoled_1_75c
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    MOTO_UI_CANVAS_WIDTH=${MOTO_BOARD_CANVAS_WIDTH}
    MOTO_UI_CANVAS_HEIGHT=${MOTO_BOARD_CANVAS_HEIGHT}
)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)
