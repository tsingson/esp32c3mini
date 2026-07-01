# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)
set(EXTRA_COMPONENT_DIRS ../lib/ssd1306idf)
set(EXTRA_COMPONENT_DIRS ../lib/ubloxm10nona)
idf_component_register(SRCS ${app_sources})
