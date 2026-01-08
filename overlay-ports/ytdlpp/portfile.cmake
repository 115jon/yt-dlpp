# ytdlpp vcpkg port - builds ytdlpp from the local source directory

# Use the source from the parent directory (when used as overlay port)
set(SOURCE_PATH "${CMAKE_CURRENT_LIST_DIR}/../..")

# Map features to CMake options INVERTED_FEATURES: when feature is ABSENT, the
# CMake option is ON
vcpkg_check_features(
    OUT_FEATURE_OPTIONS
    FEATURE_OPTIONS
    INVERTED_FEATURES
    boost
    YTDLPP_USE_SYSTEM_BOOST
    ffmpeg
    YTDLPP_USE_SYSTEM_FFMPEG
)

vcpkg_cmake_configure(
    SOURCE_PATH
    "${SOURCE_PATH}"
    OPTIONS
    -DYTDLPP_BUILD_CLI=OFF
    -DYTDLPP_BUILD_EXAMPLES=OFF
    -DYTDLPP_INSTALL=ON
    ${FEATURE_OPTIONS}
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME ytdlpp CONFIG_PATH lib/cmake/ytdlpp)
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

# Install license (create a placeholder if no LICENSE file exists)
if(EXISTS "${SOURCE_PATH}/LICENSE")
    vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
else()
    file(WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/copyright"
         "See source repository for license information."
    )
endif()
