# 足球看板:应用源码 + 预生成的中文字体子集。
#
# 基线的 demo_*.c / ui_pixel.c 仍留在仓库里(静态检查会直接编译它们做故障注入
# 测试),但不再编进本应用——本应用的界面是自己实现的,不需要测试菜单外壳。
idf_component_register(
    SRCS "main.c"
         "app_model.c"
         "app_json.c"
         "app_detail.c"
         "app_png.c"
         "app_crest.c"
         "app_nav.c"
         "app_config.c"
         "app_keyboard.c"
         "app_known.c"
         "app_store.c"
         "app_net.c"
         "app_portal.c"
         "app_console.c"
         "app_ui.c"
         "app_splash.c"
         "splash_frames.c"
         "app_fonts.c"
         "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_12.c"
         "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_15.c"
         "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_19.c"
         "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_22.c"
    INCLUDE_DIRS "."
    REQUIRES bsp
             console
             esp_event
             esp_hw_support
             esp_http_client
             esp_http_server
             esp_netif
             esp_timer
             esp_wifi
             lvgl
             mbedtls
             nvs_flash
)
