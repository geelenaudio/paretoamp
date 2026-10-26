set(priv_requires esp_timer)

if(${IDF_VERSION_MAJOR} LESS 6)
    list(APPEND priv_requires usb)       # USB PHY is part of usb component in IDF < 6.0
endif()

# Descriptors live in this component's SRCS (not injected into TinyUSB)
# so PlatformIO SCons does not compile the same file twice with different
# -D flags.
idf_component_register(SRCS usb_device_uac.c tusb/uac_usb_descriptors.c
                    INCLUDE_DIRS "include"
                    PRIV_INCLUDE_DIRS "tusb" "tusb_uac"
                    PRIV_REQUIRES ${priv_requires})

if(NOT CONFIG_USB_DEVICE_UAC_AS_PART)
        # Determine whether tinyusb is fetched from component registry or from local path
    idf_build_get_property(build_components BUILD_COMPONENTS)
    if(tinyusb IN_LIST build_components)
        set(tinyusb_name tinyusb) # Local component
    else()
        set(tinyusb_name espressif__tinyusb) # Managed component
    endif()

    idf_component_get_property(tusb_lib ${tinyusb_name} COMPONENT_LIB)
    target_include_directories(${tusb_lib} PUBLIC "${COMPONENT_DIR}/tusb" "${COMPONENT_DIR}/tusb_uac")
endif()

include(package_manager)
cu_pkg_define_version(${CMAKE_CURRENT_LIST_DIR})
