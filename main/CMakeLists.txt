idf_component_register(SRCS "tb_vital_json.c" "sx1278.c" "station_poll.c"
                            "main.c" "station_net.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_eth esp_netif esp_event nvs_flash
                             esp_driver_gpio esp_driver_spi)
# IDF v6 removed the legacy umbrella "driver" component. The header paths are
# unchanged (driver/gpio.h, driver/spi_master.h) but they now come from the
# granular esp_driver_* components, which must be required by name. Requiring
# "driver" resolves without error and then fails at compile time with
# "driver/gpio.h: No such file or directory".
#
# espressif/w5500 and espressif/mqtt come from idf_component.yml -- the
# component manager adds them to REQUIRES itself. Listing a managed component
# here fails to resolve at configure time. Neither is bundled in IDF v6.
