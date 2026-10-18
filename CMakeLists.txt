cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED ENV{COMMONLIB_SSE_FOLDER})
  message(FATAL_ERROR "Missing COMMONLIB_SSE_FOLDER environment variable")
endif()

set(AUTHOR_NAME "Parapets")
set(PRODUCT_NAME "AutoAudioSwitch")
project(${PRODUCT_NAME} VERSION 1.0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(cmake/lib/copyOutputs.cmake)
include(cmake/lib/autoCollectSources.cmake)

add_src_folder(src)
add_include_folder(include)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/version.rc @ONLY)
list(APPEND sources ${CMAKE_CURRENT_BINARY_DIR}/version.rc)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${headers} ${sources})

set(ENABLE_SKYRIM_SE OFF CACHE BOOL " " FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL " " FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL " " FORCE)
set(BUILD_TESTS OFF CACHE BOOL " " FORCE)

add_subdirectory("$ENV{COMMONLIB_SSE_FOLDER}" ${CMAKE_CURRENT_BINARY_DIR}/_deps/clib-build)
get_target_property(COMMONLIB_SRC_DIR CommonLibSSE SOURCE_DIR)
include(${COMMONLIB_SRC_DIR}/cmake/CommonLibSSE.cmake)

add_commonlibsse_plugin(${PROJECT_NAME} AUTHOR ${AUTHOR_NAME} SOURCES ${headers} ${sources})

find_path(FLICK_API_INCLUDE_DIRS "FUCK_API.h")
find_path(SKSE_MENU_FRAMEWORK_INCLUDE_DIRS "SKSEMenuFramework.h")
find_path(MODCONFIGUI_INCLUDE_DIRS "ModConfigUI/ModConfigUI.h")

set(MODCONFIGUI_SHARE_DIR "${MODCONFIGUI_INCLUDE_DIRS}/../share/modconfigui")
file(GLOB MODCONFIGUI_SOURCES "${MODCONFIGUI_SHARE_DIR}/src/ModConfigUI/*.cpp")
add_library(ModConfigUI STATIC ${MODCONFIGUI_SOURCES})
target_precompile_headers(ModConfigUI PRIVATE "${MODCONFIGUI_SHARE_DIR}/PCH.h")
target_include_directories(ModConfigUI PUBLIC ${MODCONFIGUI_INCLUDE_DIRS} PRIVATE "${MODCONFIGUI_SHARE_DIR}/src/ModConfigUI" ${FLICK_API_INCLUDE_DIRS} ${SKSE_MENU_FRAMEWORK_INCLUDE_DIRS})
target_link_libraries(ModConfigUI PUBLIC CommonLibSSE::CommonLibSSE)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE include/PCH.h)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
# CommonLibSSE targets Windows 7, this plugin targets Windows 11
target_compile_definitions(${PROJECT_NAME} PRIVATE NTDDI_VERSION=NTDDI_WIN10_CO)
target_link_libraries(${PROJECT_NAME} PRIVATE ModConfigUI mmdevapi runtimeobject)

if(DEFINED ENV{SKYRIM_MODS_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_MODS_FOLDER}")
  copyOutputs("$ENV{SKYRIM_MODS_FOLDER}/${PRODUCT_NAME}")
endif()
