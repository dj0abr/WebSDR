cmake_minimum_required(VERSION 3.20)
project(ft8_decoder VERSION 1.0.0 LANGUAGES CXX Fortran)

if(NOT CMAKE_Fortran_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR "This extraction currently supports GNU Fortran (gfortran).")
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFTW REQUIRED IMPORTED_TARGET fftw3f)
find_path(FFTW_FORTRAN_INCLUDE_DIR fftw3.f03 HINTS ${FFTW_INCLUDE_DIRS} REQUIRED)
find_package(Boost REQUIRED)

include(vendor/wsjtx/sources.cmake)
# The decoder imports these GUI IPC names but never uses them. Keep vendor
# sources byte-identical, and remove only that import in the generated copy.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS vendor/wsjtx/lib/ft8_decode.f90)
file(READ vendor/wsjtx/lib/ft8_decode.f90 DECODER_SOURCE)
string(REPLACE "    use shmem, only: shmem_lock, shmem_unlock\n" "" DECODER_SOURCE "${DECODER_SOURCE}")
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/ft8_decode.f90 "${DECODER_SOURCE}")

add_library(ft8_core STATIC ${WSJTX_SOURCES}
  ${CMAKE_CURRENT_BINARY_DIR}/generated/ft8_decode.f90 src/ft8_bridge.f90)
set_target_properties(ft8_core PROPERTIES Fortran_MODULE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/modules)
target_include_directories(ft8_core PRIVATE vendor/wsjtx/lib vendor/wsjtx/lib/ft8
  ${FFTW_FORTRAN_INCLUDE_DIR} ${Boost_INCLUDE_DIRS} ${CMAKE_CURRENT_BINARY_DIR}/modules)
target_compile_options(ft8_core PRIVATE
  $<$<COMPILE_LANGUAGE:Fortran>:-fallow-argument-mismatch;-ffree-line-length-none;-fno-second-underscore>)
target_link_libraries(ft8_core PUBLIC PkgConfig::FFTW)

add_executable(ft8-decoder src/main.cpp)
target_compile_features(ft8-decoder PRIVATE cxx_std_17)
target_compile_options(ft8-decoder PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(ft8-decoder PRIVATE ft8_core)
include(GNUInstallDirs)
install(TARGETS ft8-decoder RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES COPYING README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_executable(ft8-test-signal tests/make_signal.f90)
  target_include_directories(ft8-test-signal PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/modules)
  target_link_libraries(ft8-test-signal PRIVATE ft8_core)
  add_test(NAME ft8-integration COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py $<TARGET_FILE:ft8-decoder>
    $<TARGET_FILE:ft8-test-signal> ${CMAKE_CURRENT_SOURCE_DIR}/tests/data)
  set_tests_properties(ft8-integration PROPERTIES TIMEOUT 180)
endif()
