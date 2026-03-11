FILE(GLOB_RECURSE app_sources ${CMAKE_CURRENT_SOURCE_DIR}/*.c)

set(COMPONENT_SRCS ${app_sources})
set(COMPONENT_ADD_INCLUDEDIRS ".")
set(COMPONENT_REQUIRES spiffs)
register_component()

target_add_binary_data(${COMPONENT_TARGET} "web_ui.html" TEXT)
target_add_binary_data(${COMPONENT_TARGET} "calibrate.html" TEXT)
target_add_binary_data(${COMPONENT_TARGET} "server.crt" TEXT)
target_add_binary_data(${COMPONENT_TARGET} "server.key" TEXT)