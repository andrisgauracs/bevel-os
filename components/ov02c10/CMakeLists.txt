# OV02C10 image sensor driver for esp_cam_sensor (the registry's esp_cam_sensor has none for this
# sensor). Taken from the copy in p4_edge_enhancer, which got it with esp_cam_sensor 2.1 and fixed
# the mirror bit there (0x3820 bit 3, see ov02c10_set_mirror). Only the P4 build has the camera
# framework (main/idf_component.yml); on other chips this component is empty. The requirements go by
# the target because ESP-IDF works them out before the CONFIG_ values are known.
if(NOT IDF_TARGET STREQUAL "esp32p4")
    idf_component_register()
    return()
endif()

set(srcs)
if(CONFIG_CAMERA_OV02C10)
    list(APPEND srcs "ov02c10.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "private_include"
                       REQUIRES espressif__esp_cam_sensor
                       PRIV_REQUIRES esp_driver_gpio espressif__esp_sccb_intf)

# the detect function sits in a linker section that esp_cam_sensor walks at start-up; nothing calls
# it by name, so keep the linker from dropping it
if(CONFIG_CAMERA_OV02C10_AUTO_DETECT_MIPI_INTERFACE_SENSOR OR CONFIG_CAMERA_OV02C10_AUTO_DETECT_DVP_INTERFACE_SENSOR)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u ov02c10_detect")
endif()
