idf_component_register(
    SRCS
        "dsi_camera_preview_main.c"
        "app_camera.c"
        "app_display.c"
        "app_scaler.c"
        "app_sensor.c"
        "app_snapshot.c"
        "app_stats.c"
        "app_ui.c"
    INCLUDE_DIRS "."
    REQUIRES epdinky_p4_board esp_lcd esp_lcd_jd9168 sgm37604a tca6408 esp_mm
             esp_driver_jpeg esp_driver_ppa fatfs
)

# app_sensor.c needs esp_video's private headers to reach the camera sensor
# handle, which is the only way to switch sensor modes at runtime. See the note
# at the top of that file for why there is no supported alternative.
idf_component_get_property(esp_video_dir espressif__esp_video COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${esp_video_dir}/private_include")
