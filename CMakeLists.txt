########################################################################
# Project setup -- only needed if device support is a stand-alone build
# We recommend that the support module be built in-tree with the driver.
########################################################################
cmake_minimum_required(VERSION 3.10)
project(SoapyRX888 CXX)


#select the release build type by default to get optimization flags
if(NOT CMAKE_BUILD_TYPE)
   set(CMAKE_BUILD_TYPE "Release")
   message(STATUS "Build type not specified: defaulting to release.")
endif(NOT CMAKE_BUILD_TYPE)
set(CMAKE_BUILD_TYPE ${CMAKE_BUILD_TYPE} CACHE STRING "")

########################################################################
# build the module
########################################################################
find_package(SoapySDR CONFIG)

if (NOT SoapySDR_FOUND)
    message(WARNING "SoapySDR development files not found - skipping support")
    return()
endif ()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR})
find_package(RX888)
if (NOT RX888_FOUND)
    message(FATAL_ERROR "RX888 development files not found...")
endif ()
message(STATUS "RX888_INCLUDE_DIRS - ${RX888_INCLUDE_DIRS}")
message(STATUS "RX888_LIBRARIES - ${RX888_LIBRARIES}")

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${RX888_INCLUDE_DIRS})

# Test for Atomics
include(CheckAtomic)
if(NOT HAVE_CXX_ATOMICS_WITHOUT_LIB OR NOT HAVE_CXX_ATOMICS64_WITHOUT_LIB)
    set(ATOMIC_LIBS "atomic")
endif()

#C++11 is a required language feature for this project
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

#Thread support enabled (not the same as -lpthread)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

if(CMAKE_COMPILER_IS_GNUCXX)
    #disable warnings for unused parameters
    add_definitions(-Wno-unused-parameter)
endif(CMAKE_COMPILER_IS_GNUCXX)

SOAPY_SDR_MODULE_UTIL(
    TARGET rx888Support
    SOURCES
        SoapyRX888.hpp
        Registration.cpp
        Settings.cpp
        Streaming.cpp
    LIBRARIES
        ${RX888_LIBRARIES}
        ${ATOMIC_LIBS}
        Threads::Threads
)
