# The original Card Craft game code lives unchanged in game/. world/raycast/game/render
# are compiled as one translation unit through game/unity.cpp, as upstream does.
idf_component_register(
    SRCS
        "app_main.cpp"
        "hal_tab5.cpp"
        "usb_kbd.cpp"
        "tab5_kbd.cpp"
        "audio_tab5.cpp"
        "compat/compat.cpp"
        "game/unity.cpp"
        "game/main.cpp"
        "game/ui.cpp"
        "game/sfx.cpp"
    INCLUDE_DIRS "." "compat" "game"
    REQUIRES m5stack_tab5 esp_lcd esp_timer nvs_flash esp_driver_gpio esp_driver_i2c
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_TAB5=1)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -O3 -falign-functions=16 -falign-loops=16
    -Wno-error -Wno-unused-function -Wno-missing-field-initializers)
set_source_files_properties(
    game/unity.cpp game/main.cpp game/ui.cpp game/sfx.cpp
    PROPERTIES COMPILE_OPTIONS "-ffast-math")
