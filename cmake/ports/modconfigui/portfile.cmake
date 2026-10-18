vcpkg_from_github(OUT_SOURCE_PATH SOURCE_PATH REPO Vermunds/ModConfigUI REF d88a7f2bd6aa6bf9789018008b6b5854f24bcfae SHA512 2281beec7d3a85907710f908fdf7ee1121f00d3ddeb850987d7447004eb46ad97bb4ecb6b21b9f4d89738b774c5af6322167ce09de43d3205ec2dafc610b7510 HEAD_REF master)

# CommonLibSSE-NG already has SKSE::log, the shim for libxse's CommonLib collides with it
file(READ "${SOURCE_PATH}/src/ModConfigUI/FUCKBackend.cpp" FUCK_BACKEND)
string(REGEX REPLACE "namespace SKSE::log\n{[^}]*}\n" "" FUCK_BACKEND "${FUCK_BACKEND}")
if(FUCK_BACKEND MATCHES "namespace SKSE::log")
  message(FATAL_ERROR "FUCKBackend.cpp SKSE::log shim not removed")
endif()
file(WRITE "${SOURCE_PATH}/src/ModConfigUI/FUCKBackend.cpp" "${FUCK_BACKEND}")

file(INSTALL "${SOURCE_PATH}/include/ModConfigUI" DESTINATION "${CURRENT_PACKAGES_DIR}/include")
file(INSTALL "${SOURCE_PATH}/src/ModConfigUI" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}/src")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/PCH.h" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE" "${SOURCE_PATH}/EXCEPTIONS.md")
