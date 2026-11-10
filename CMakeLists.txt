cmake_minimum_required(VERSION 3.20)
project(latch VERSION 0.1.0 LANGUAGES CXX)

# latch -- the shared door (2026-11-10). Split out of Rouster so Rouster and
# Tidy move files through ONE door: journal, manifest, identity-checked undo,
# the Protected list, packages-are-one-thing. GTK-free, no spdlog.
#
# A STATIC library: each app compiles the door into itself, so there is no .so
# at run time and each app's selftest checks the exact door it ships.
#
# Two ways an app gets it:
#   installed  -- ./install.sh puts headers + liblatch.a + a CMake package in
#                 /usr/local; the app says find_package(latch) and links
#                 latch::latch.
#   from source -- the app add_subdirectory()s this folder; same target name.
if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(latch STATIC
    # THE ONE DOOR: the only file that changes a user's files. Each consumer's
    # selftest source gate allows it (and Pipe.cpp's write() into child pipes).
    src/Door.cpp
    src/Protect.cpp
    src/Package.cpp
    src/Vcs.cpp
    src/Pipe.cpp
)
add_library(latch::latch ALIAS latch)
target_include_directories(latch PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# json.hpp is latch's own business; no consumer inherits the include.
target_include_directories(latch PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>)
target_compile_features(latch PUBLIC cxx_std_23)
set_target_properties(latch PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ── install: headers, liblatch.a, and the CMake package (find_package(latch)) ──
install(TARGETS latch EXPORT latchTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/latch DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT latchTargets NAMESPACE latch::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/latch)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/latchConfig.cmake
     "include(\${CMAKE_CURRENT_LIST_DIR}/latchTargets.cmake)\n")
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/latchConfigVersion.cmake
     VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/latchConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/latchConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/latch)
