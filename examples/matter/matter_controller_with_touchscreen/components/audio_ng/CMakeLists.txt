idf_component_register(
    SRC_DIRS .
    EMBED_FILES wakeup.opus stop.opus
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS .
    REQUIRES esp_board_manager esp_psram
)
