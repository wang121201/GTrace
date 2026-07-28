cmake_minimum_required(VERSION 3.15)
project(GTSim VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_library(gtsim_core STATIC
  src/scheduler.cpp
  src/subpartition.cpp
  src/sm.cpp
  src/gpu.cpp
  src/simulator.cpp)
target_include_directories(gtsim_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(gtsim_gemm workloads/gemm/gemm_h100.cpp)
target_link_libraries(gtsim_gemm PRIVATE gtsim_core)

add_executable(gtsim_fa3 workloads/fa3/fa3_h100.cpp)
target_link_libraries(gtsim_fa3 PRIVATE gtsim_core)

include(CTest)
if(BUILD_TESTING)
  add_test(NAME gemm_smoke COMMAND gtsim_gemm 512 512 512)
  add_test(NAME fa3_smoke COMMAND gtsim_fa3 1 1 1 128 128)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME workload_profile_boundary
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_workload_profile.py)
    add_test(NAME v0_1_0_regression
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_reference.py
              --build-dir ${CMAKE_CURRENT_BINARY_DIR} --verify)
  endif()
endif()

install(TARGETS gtsim_core gtsim_gemm gtsim_fa3
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include/gtsim)
