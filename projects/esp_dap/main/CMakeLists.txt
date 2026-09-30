idf_component_register(
    SRCS "main.c" "swd.c" "dap.c" "usb_descriptors.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_tinyusb esp_driver_uart esp_driver_gpio esp_ringbuf freertos
)
