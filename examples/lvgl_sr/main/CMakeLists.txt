idf_component_register(
    SRCS "app_main.c" "sr_owner.c"
    INCLUDE_DIRS "."
    REQUIRES esp-sr esp_board_manager esp_codec_dev esp_lvgl_adapter esp_timer gmf_ai_audio gmf_core lvgl
)
