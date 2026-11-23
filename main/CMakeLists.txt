idf_component_register(SRCS "main.c"
                            "epd4in2.c"
                            "canvas_epd.c"
                            "clock_ui.c"
                            "font_bar.c"
                            "font_status.c"
                            "font_text.c"
                            "quotes.c"
                            "art.c"
                            "web_portal.c"
                            "ansi_art.c"
                            "glyphfont.c"
                            "net_time.c"
                            "weather.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_spi esp_driver_gpio esp_wifi esp_netif
                                esp_event nvs_flash lwip esp_http_server
                                esp_http_client esp_timer)
