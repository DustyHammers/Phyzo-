cmake_minimum_required(VERSION 3.16)
project(phyzo_rombuilder CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
add_executable(rombuilder src/rombuilder.cpp)
add_executable(refplayer src/refplayer.cpp)

# Test that needs no ROM data: builds a ROM from inputs generated at run time (run by CI).
enable_testing()
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME synthetic_rom
           COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tests/synthetic_rom_test.py
                   $<TARGET_FILE:rombuilder> $<TARGET_FILE:refplayer>)
endif()
