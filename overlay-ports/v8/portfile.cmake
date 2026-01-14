vcpkg_from_github(
  OUT_SOURCE_PATH
  SOURCE_PATH
  REPO
  bnoordhuis/v8-cmake
  REF
  3c346df2815811c3470412ea0c490985fa768fd3
  SHA512
  eef9b107d45dd978a54ff5ea8e54cc778fef55e9353b363f7a82373cb20a0e387007fd4849b3e51fdaac3d0e07bca72d787c50a3e754ba26648faa335edde9e4
  PATCHES
  v8-cmake-features.patch)

# =============================================================================
# Feature Detection
# =============================================================================
string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" V8_BUILD_SHARED)

if("monolithic" IN_LIST FEATURES)
  set(V8_MONOLITHIC ON)
else()
  set(V8_MONOLITHIC OFF)
endif()

# =============================================================================
# Build Options
# =============================================================================
set(OPTIONS -DV8_ENABLE_I18N=OFF -DV8_BUILD_SHARED=${V8_BUILD_SHARED}
            -DV8_MONOLITHIC=${V8_MONOLITHIC} -DV8_BUILD_SAMPLES=OFF)

# ARM64 cross-compilation hint
if(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
  list(APPEND OPTIONS -DCMAKE_HOST_SYSTEM_PROCESSOR=aarch64)
endif()

# Suppress warnings-as-errors on non-Windows (V8 has many warnings) Use lld
# linker to avoid GNU ld memory exhaustion on large builds
if(NOT VCPKG_TARGET_IS_WINDOWS)
  set(VCPKG_CXX_FLAGS "${VCPKG_CXX_FLAGS} -Wno-error")
  set(VCPKG_C_FLAGS "${VCPKG_C_FLAGS} -Wno-error")
  # Use lld via -B to specify linker search path - much more memory-efficient
  # than GNU ld
  set(linker_flags "-B/usr/bin -fuse-ld=lld")
  list(APPEND OPTIONS "-DCMAKE_SHARED_LINKER_FLAGS=${linker_flags}")
  list(APPEND OPTIONS "-DCMAKE_EXE_LINKER_FLAGS=${linker_flags}")
  list(APPEND OPTIONS "-DCMAKE_MODULE_LINKER_FLAGS=${linker_flags}")
endif()

if(VCPKG_TARGET_IS_WINDOWS)
  # Use FASTLINK PDB format for debug builds to avoid LNK1201 PDB write errors
  # FASTLINK creates smaller PDB by referencing .obj files instead of copying
  # all debug info This prevents PDB size issues with V8's massive codebase
  list(APPEND OPTIONS
       "-DCMAKE_SHARED_LINKER_FLAGS_DEBUG=/DEBUG:FASTLINK /INCREMENTAL:NO")
  list(APPEND OPTIONS
       "-DCMAKE_EXE_LINKER_FLAGS_DEBUG=/DEBUG:FASTLINK /INCREMENTAL:NO")
  list(APPEND OPTIONS
       "-DCMAKE_MODULE_LINKER_FLAGS_DEBUG=/DEBUG:FASTLINK /INCREMENTAL:NO")
endif()

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}" OPTIONS ${OPTIONS})

vcpkg_cmake_build()
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME v8 CONFIG_PATH lib/cmake/v8)
vcpkg_copy_pdbs()

# =============================================================================
# Cleanup
# =============================================================================
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

if(NOT V8_BUILD_SHARED)
  file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/bin"
       "${CURRENT_PACKAGES_DIR}/debug/bin")
endif()

# =============================================================================
# Copyright
# =============================================================================
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/v8/LICENSE")
