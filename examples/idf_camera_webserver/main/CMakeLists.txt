idf_component_register(
    SRCS
        "camera_webserver_main.c"
        "app_camera.c"
        "app_h264.c"
        "app_httpd.c"
        "app_power.c"
        "app_scaler.c"
        "app_sensor.c"
        "app_snapshot.c"
        "rtsp_server.c"
    INCLUDE_DIRS "."
    REQUIRES epdinky_p4_board esp_http_server esp_driver_jpeg esp_driver_ppa
    EMBED_FILES "www/index.html"
)

# app_sensor.c needs esp_video's private headers to reach the camera sensor
# handle, which is the only way to switch sensor modes at runtime. See the note
# at the top of that file for why there is no supported alternative.
idf_component_get_property(esp_video_dir espressif__esp_video COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${esp_video_dir}/private_include")
