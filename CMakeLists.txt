cmake_minimum_required(VERSION 3.20)
project(JordanType VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

set(JORDAN_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(JORDAN_TOP_LEVEL ON)
endif()
option(JORDAN_BUILD_TESTS "Build mathematical and command-line tests" ${JORDAN_TOP_LEVEL})
option(JORDAN_BUILD_CLI "Build the presentation-file command-line program" ${JORDAN_TOP_LEVEL})
set(JORDAN_ALGEBRA_SOURCE_DIR "" CACHE PATH "Existing pinned Persistence-Algebra source directory")
set(JORDAN_DOCTEST_SOURCE_DIR "" CACHE PATH "Existing doctest source directory")

# Only the arithmetic headers are needed. The dependency's own tools, examples,
# module framework, and tests are deliberately not added to this build.
if(NOT JORDAN_ALGEBRA_SOURCE_DIR)
  FetchContent_Declare(jordan_algebra
    URL https://github.com/JanJend/Persistence-Algebra/archive/d9b85f4bf692b0ada09e2c43897685b83a2b705c.tar.gz
    URL_HASH SHA256=10410895de44c8477bbe1c899488f8f4a9d1ee977ba73e0691ed0119411a1ab2
    SOURCE_SUBDIR jordan_headers_only)
  FetchContent_MakeAvailable(jordan_algebra)
  set(JORDAN_ALGEBRA_SOURCE_DIR "${jordan_algebra_SOURCE_DIR}")
endif()
if(NOT EXISTS "${JORDAN_ALGEBRA_SOURCE_DIR}/include/grlina/matrices/sparse.hpp")
  message(FATAL_ERROR "JORDAN_ALGEBRA_SOURCE_DIR does not contain the required arithmetic headers")
endif()
find_package(Boost REQUIRED)
find_package(Threads REQUIRED)

add_library(jordan_core STATIC src/jordan.cpp src/io.cpp)
add_library(Jordan::core ALIAS jordan_core)
set_target_properties(jordan_core PROPERTIES
  EXPORT_NAME core
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON)
target_compile_features(jordan_core PUBLIC cxx_std_20)
target_link_libraries(jordan_core PRIVATE Threads::Threads)
target_include_directories(jordan_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(jordan_core SYSTEM PRIVATE
  "${JORDAN_ALGEBRA_SOURCE_DIR}/include" ${Boost_INCLUDE_DIRS})
if(MSVC)
  target_compile_options(jordan_core PRIVATE /W4)
else()
  target_compile_options(jordan_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(JORDAN_BUILD_CLI)
  add_executable(jordan_type apps/jordan_type.cpp)
  target_link_libraries(jordan_type PRIVATE Jordan::core)
  install(TARGETS jordan_type RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(JORDAN_BUILD_TESTS)
  enable_testing()
  if(NOT JORDAN_DOCTEST_SOURCE_DIR)
    FetchContent_Declare(jordan_doctest
      URL https://github.com/doctest/doctest/archive/2d0a9359a60c51affe2a9bebb1be1dca47868151.tar.gz
      URL_HASH SHA256=e64542c4ea68e9f381ccf6eae924cfdd652567c87c142d76fe92644fb4608149
      SOURCE_SUBDIR jordan_headers_only)
    FetchContent_MakeAvailable(jordan_doctest)
    set(JORDAN_DOCTEST_SOURCE_DIR "${jordan_doctest_SOURCE_DIR}")
  endif()
  add_executable(jordan_tests tests/test_jordan.cpp tests/test_io.cpp)
  target_compile_definitions(jordan_tests PRIVATE
    JORDAN_TEST_CASES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/cases")
  target_include_directories(jordan_tests SYSTEM PRIVATE "${JORDAN_DOCTEST_SOURCE_DIR}")
  # The reader compatibility test uses the dependency's independent SCC writer.
  target_include_directories(jordan_tests SYSTEM PRIVATE
    "${JORDAN_ALGEBRA_SOURCE_DIR}/include" ${Boost_INCLUDE_DIRS})
  target_link_libraries(jordan_tests PRIVATE Jordan::core)
  add_test(NAME jordan_mathematics COMMAND jordan_tests)

  if(JORDAN_BUILD_CLI)
    add_test(NAME jordan_cli COMMAND ${CMAKE_COMMAND}
      "-DPROGRAM=$<TARGET_FILE:jordan_type>"
      "-DEXAMPLES=${CMAKE_CURRENT_SOURCE_DIR}/examples"
      "-DCASES=${CMAKE_CURRENT_SOURCE_DIR}/tests/cases"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli.cmake")
  endif()
endif()

# Timing programs are ordinary targets, built only when explicitly requested.
# Both use the ordinary Release build.
add_executable(jordan_benchmark EXCLUDE_FROM_ALL benchmarks/query_benchmark.cpp)
target_link_libraries(jordan_benchmark PRIVATE Jordan::core)
add_executable(jordan_method_benchmark EXCLUDE_FROM_ALL benchmarks/method_benchmark.cpp)
target_link_libraries(jordan_method_benchmark PRIVATE Jordan::core)
foreach(target IN ITEMS jordan_benchmark jordan_method_benchmark)
  target_compile_definitions(${target} PRIVATE JORDAN_BENCHMARK_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
endforeach()

# Install the reusable library interface and package metadata. Examples,
# tests, documentation and benchmark evidence stay in the source tree.
install(TARGETS jordan_core EXPORT JordanTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT JordanTargets NAMESPACE Jordan::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/JordanType)
configure_package_config_file(cmake/JordanTypeConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/JordanTypeConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/JordanType)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/JordanTypeConfigVersion.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/JordanTypeConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/JordanTypeConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/JordanType)
