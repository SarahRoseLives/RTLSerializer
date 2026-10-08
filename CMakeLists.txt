cmake_minimum_required(VERSION 3.20)

project(RTLSerializer LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Vendored RTL-SDR SDK (librtlsdr headers, import libraries and runtime DLLs).
set(RTLSDK "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rtlsdr")

add_executable(RTLSerializer WIN32
    src/main.cpp
    src/eeprom.cpp
    src/app.rc
)

target_include_directories(RTLSerializer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${RTLSDK}/include"
)

# Make the resource compiler find app.manifest next to app.rc.
set_source_files_properties(src/app.rc PROPERTIES
    COMPILE_FLAGS "-I${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(RTLSerializer PRIVATE
    "${RTLSDK}/lib/rtlsdr.lib"
    comctl32
    comdlg32
    shell32
    user32
    gdi32
    ole32
    uuid
)

# Statically link the GCC runtime so only the RTL-SDR DLLs are needed at runtime.
if(MINGW)
    target_link_options(RTLSerializer PRIVATE
        -static-libgcc
        -static-libstdc++
        -static
    )
endif()

# Copy the RTL-SDR runtime DLLs next to the executable.
add_custom_command(TARGET RTLSerializer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RTLSDK}/bin/rtlsdr.dll"
            "$<TARGET_FILE_DIR:RTLSerializer>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RTLSDK}/bin/libusb-1.0.dll"
            "$<TARGET_FILE_DIR:RTLSerializer>"
    COMMENT "Copying RTL-SDR runtime DLLs"
)
