# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Rack Fabric build.
#
# The project is standalone: it requires no other Summon Software Labs
# repository to build its core, and it has no third-party dependencies.

cmake_minimum_required(VERSION 3.20)

project(
  RackFabric
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral rack-level infrastructure runtime"
  HOMEPAGE_URL "https://github.com/pngen/Rack-Fabric"
  LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(RACK_FABRIC_IS_TOP_LEVEL ON)
else()
  set(RACK_FABRIC_IS_TOP_LEVEL OFF)
endif()

option(RACK_FABRIC_BUILD_LIBRARY "Build the rack_fabric library" ON)
option(RACK_FABRIC_BUILD_TOOLS "Build the coordinator, agent and inspection tools" ON)
option(RACK_FABRIC_BUILD_TESTS "Build the Rack Fabric test suites" ON)
option(RACK_FABRIC_BUILD_EXAMPLES "Build the examples" ON)
option(RACK_FABRIC_BUILD_BENCHMARKS "Build the benchmarks" ON)
option(RACK_FABRIC_ENABLE_CUDA "Build the CUDA accelerator evidence module when a CUDA toolkit is found" ON)
option(RACK_FABRIC_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(RACK_FABRIC_ENABLE_ASAN "Build with AddressSanitizer" OFF)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Declared after GNUInstallDirs: CMAKE_INSTALL_LIBDIR is only defined once that
# module has run, and an empty component would turn the destination into an
# absolute path outside the install prefix.
set(RACK_FABRIC_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/rack_fabric"
    CACHE STRING "Directory for the installed CMake package configuration")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Warning configuration: strict for first-party code only.
# ---------------------------------------------------------------------------

add_library(rack_fabric_warnings INTERFACE)
if(MSVC)
  target_compile_options(rack_fabric_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
  if(RACK_FABRIC_WARNINGS_AS_ERRORS)
    target_compile_options(rack_fabric_warnings INTERFACE /WX)
  endif()
  if(RACK_FABRIC_ENABLE_ASAN)
    target_compile_options(rack_fabric_warnings INTERFACE /fsanitize=address)
    target_link_options(rack_fabric_warnings INTERFACE /fsanitize=address)
  endif()
  target_compile_definitions(rack_fabric_warnings INTERFACE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX)
else()
  target_compile_options(rack_fabric_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wnon-virtual-dtor)
  if(RACK_FABRIC_WARNINGS_AS_ERRORS)
    target_compile_options(rack_fabric_warnings INTERFACE -Werror)
  endif()
  if(RACK_FABRIC_ENABLE_ASAN)
    target_compile_options(rack_fabric_warnings INTERFACE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(rack_fabric_warnings INTERFACE -fsanitize=address)
  endif()
endif()

# ---------------------------------------------------------------------------
# The library
# ---------------------------------------------------------------------------

set(RACK_FABRIC_SOURCES
  src/core/canonical.cpp
  src/core/model.cpp
  src/core/rack_fabric.cpp
  src/core/state.cpp
  src/internal/crypto.cpp
  src/internal/files.cpp
  src/internal/log.cpp
  src/persistence/persistence.cpp
  src/protocol/protocol.cpp
  src/synthetic/synthetic.cpp
  src/hardware/hardware.cpp
  src/transport/tcp.cpp
  src/coordinator/coordinator.cpp
  src/agent/agent.cpp)

add_library(rack_fabric STATIC ${RACK_FABRIC_SOURCES})
add_library(rack_fabric::rack_fabric ALIAS rack_fabric)

target_include_directories(rack_fabric
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

# BUILD_INTERFACE keeps the warning interface out of the installed export set:
# consumers of the package never inherit first-party build flags.
target_link_libraries(rack_fabric PRIVATE $<BUILD_INTERFACE:rack_fabric_warnings>)
target_compile_features(rack_fabric PUBLIC cxx_std_20)

set_target_properties(rack_fabric PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  EXPORT_NAME rack_fabric)

if(WIN32)
  target_link_libraries(rack_fabric PRIVATE ws2_32 iphlpapi setupapi advapi32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(rack_fabric PRIVATE Threads::Threads)
endif()

target_compile_definitions(rack_fabric PRIVATE RACK_FABRIC_BUILDING_LIBRARY=1)

# ---------------------------------------------------------------------------
# CUDA accelerator evidence (optional, real hardware proof)
# ---------------------------------------------------------------------------

if(RACK_FABRIC_ENABLE_CUDA)
  include(CheckLanguage)
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    add_library(rack_fabric_cuda STATIC
      src/cuda/cuda_probe.cpp
      src/cuda/proof_kernel.cu)
    add_library(rack_fabric::rack_fabric_cuda ALIAS rack_fabric_cuda)
    target_include_directories(rack_fabric_cuda
      PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
      PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(rack_fabric_cuda PUBLIC rack_fabric)
    # The warning interface cannot be linked here: nvcc would receive the raw
    # MSVC flags as nvcc options. The host compiler is configured explicitly
    # instead, so the CUDA translation unit is held to the same standard as
    # every other first-party translation unit.
    set_target_properties(rack_fabric_cuda PROPERTIES
      CUDA_STANDARD 20
      CUDA_STANDARD_REQUIRED ON
      CXX_STANDARD 20
      CXX_STANDARD_REQUIRED ON
      CUDA_ARCHITECTURES "native"
      CUDA_SEPARABLE_COMPILATION OFF
      EXPORT_NAME rack_fabric_cuda)
    if(MSVC)
      target_compile_options(rack_fabric_cuda PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/W4,/permissive-,/utf-8,/Zc:__cplusplus,/EHsc,/WX>
        $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/wd4068,/wd4211>
        $<$<COMPILE_LANGUAGE:CXX>:/W4>
        $<$<COMPILE_LANGUAGE:CXX>:/permissive->
        $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
        $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>
        $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
        $<$<COMPILE_LANGUAGE:CXX>:/WX>
        $<$<COMPILE_LANGUAGE:CXX>:/wd4068>)
    endif()
    set(RACK_FABRIC_CUDA_AVAILABLE ON)
    message(STATUS "Rack Fabric: CUDA toolkit found at ${CMAKE_CUDA_COMPILER}")
  else()
    set(RACK_FABRIC_CUDA_AVAILABLE OFF)
    message(STATUS "Rack Fabric: no CUDA toolkit found; CUDA evidence module disabled")
  endif()
else()
  set(RACK_FABRIC_CUDA_AVAILABLE OFF)
endif()

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------

if(RACK_FABRIC_BUILD_TOOLS)
  add_executable(rack_fabric_coordinator tools/coordinator_main.cpp)
  target_link_libraries(rack_fabric_coordinator PRIVATE rack_fabric rack_fabric_warnings)

  add_executable(rack_fabric_agent tools/agent_main.cpp)
  target_link_libraries(rack_fabric_agent PRIVATE rack_fabric rack_fabric_warnings)

  add_executable(rack_fabric_inspect tools/inspect_main.cpp)
  target_link_libraries(rack_fabric_inspect PRIVATE rack_fabric rack_fabric_warnings)
  if(RACK_FABRIC_CUDA_AVAILABLE)
    target_link_libraries(rack_fabric_inspect PRIVATE rack_fabric_cuda)
    target_compile_definitions(rack_fabric_inspect PRIVATE RACK_FABRIC_HAS_CUDA=1)
  endif()

  install(TARGETS rack_fabric_coordinator rack_fabric_agent rack_fabric_inspect
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ---------------------------------------------------------------------------
# Tests, examples, benchmarks
# ---------------------------------------------------------------------------

if(RACK_FABRIC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(RACK_FABRIC_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(RACK_FABRIC_BUILD_BENCHMARKS)
  add_executable(rack_fabric_benchmarks benchmarks/benchmarks.cpp)
  target_link_libraries(rack_fabric_benchmarks PRIVATE rack_fabric rack_fabric_warnings)
  if(RACK_FABRIC_CUDA_AVAILABLE)
    target_link_libraries(rack_fabric_benchmarks PRIVATE rack_fabric_cuda)
  endif()
endif()

# ---------------------------------------------------------------------------
# Install and package export
# ---------------------------------------------------------------------------

install(TARGETS rack_fabric
        EXPORT rack_fabricTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(RACK_FABRIC_CUDA_AVAILABLE)
  install(TARGETS rack_fabric_cuda
          EXPORT rack_fabricTargets
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
          INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT rack_fabricTargets
        FILE rack_fabricTargets.cmake
        NAMESPACE rack_fabric::
        DESTINATION ${RACK_FABRIC_INSTALL_CMAKEDIR})

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/rack_fabricConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/rack_fabricConfig.cmake
  INSTALL_DESTINATION ${RACK_FABRIC_INSTALL_CMAKEDIR})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/rack_fabricConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/rack_fabricConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/rack_fabricConfigVersion.cmake
        DESTINATION ${RACK_FABRIC_INSTALL_CMAKEDIR})

install(FILES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
