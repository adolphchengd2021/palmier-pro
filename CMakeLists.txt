cmake_minimum_required(VERSION 3.31)

project(PalmierProWindowsContracts LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The Palmier Pro Windows probe requires Windows.")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "The Palmier Pro Windows probe requires MSVC.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The Palmier Pro Windows probe requires an x64 toolchain.")
endif()

include(CTest)
add_subdirectory(windows/contract-probe)
