cmake_minimum_required(VERSION 3.22.1)

# LURVETA: package identity injected by Gradle (-DAPP_ID=...).
if(NOT DEFINED APP_ID)
    message(FATAL_ERROR "APP_ID must be provided by Gradle")
endif()
add_compile_definitions(APP_ID="${APP_ID}")

add_subdirectory(winlator)
add_subdirectory(vortekrenderer)
add_subdirectory(virglrenderer)
add_subdirectory(midihandler)
add_subdirectory(gladiorenderer)