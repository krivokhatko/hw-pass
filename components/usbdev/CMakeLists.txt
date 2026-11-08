# The USB device needs the USB OTG of the chip: the ESP32-S3 has it, the
# ESP32 has not. A chip without it builds usbdev_none.c: no device, and the
# functions say so. The requirements cannot depend on the configuration,
# but they can depend on the chip.
idf_build_get_property(target IDF_TARGET)
if(target STREQUAL "esp32s2" OR target STREQUAL "esp32s3" OR target STREQUAL "esp32p4")
    set(src "usbdev.c")
    set(usb esp_tinyusb)
else()
    set(src "usbdev_none.c")
    set(usb "")
endif()

idf_component_register(SRCS ${src}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES ${usb} esp_hw_support esp_timer hal nvs_flash soc)
