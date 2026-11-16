cmake_minimum_required(VERSION 3.15)
project(tu_pccscc LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PREDICATES_LLVM_ROOT "" CACHE PATH
    "Optional LLVM/Clang install prefix, e.g. /home/user/clang-dev/install")
set(PREDICATES_LLVM_BUILD_DIR "" CACHE PATH
    "Optional LLVM build directory used to locate LLVM's static GTest libraries")
set(PREDICATES_LLVM_SOURCE_DIR "" CACHE PATH
    "Optional llvm-project/llvm source directory used to locate GTest headers")
set(PREDICATES_LLVM_GTEST_LIB "" CACHE FILEPATH
    "Optional path to libllvm_gtest.a")
set(PREDICATES_GTEST_INCLUDE_DIR "" CACHE PATH
    "Optional path to LLVM's googletest include directory")

if(PREDICATES_LLVM_ROOT)
    list(APPEND CMAKE_PREFIX_PATH "${PREDICATES_LLVM_ROOT}")
endif()

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

if(NOT EXISTS "${PREDICATES_LLVM_GTEST_LIB}" AND PREDICATES_LLVM_BUILD_DIR)
    find_library(PREDICATES_DETECTED_LLVM_GTEST_LIB
        NAMES llvm_gtest
        PATHS "${PREDICATES_LLVM_BUILD_DIR}/lib"
        NO_DEFAULT_PATH)
    if(PREDICATES_DETECTED_LLVM_GTEST_LIB)
        set(PREDICATES_LLVM_GTEST_LIB "${PREDICATES_DETECTED_LLVM_GTEST_LIB}"
            CACHE FILEPATH "Optional path to libllvm_gtest.a" FORCE)
    endif()
endif()

if(NOT EXISTS "${PREDICATES_GTEST_INCLUDE_DIR}" AND PREDICATES_LLVM_SOURCE_DIR)
    get_filename_component(LLVM_PROJECT_ROOT
        "${PREDICATES_LLVM_SOURCE_DIR}/.." ABSOLUTE)
    find_path(PREDICATES_DETECTED_GTEST_INCLUDE_DIR
        NAMES gtest/gtest.h
        PATHS
            "${LLVM_PROJECT_ROOT}/third-party/unittest/googletest/include"
            "${PREDICATES_LLVM_SOURCE_DIR}/../third-party/unittest/googletest/include"
        NO_DEFAULT_PATH)
    if(PREDICATES_DETECTED_GTEST_INCLUDE_DIR)
        set(PREDICATES_GTEST_INCLUDE_DIR
            "${PREDICATES_DETECTED_GTEST_INCLUDE_DIR}"
            CACHE PATH "Optional path to LLVM's googletest include directory" FORCE)
    endif()
endif()

if(EXISTS "${PREDICATES_LLVM_GTEST_LIB}" AND
   EXISTS "${PREDICATES_GTEST_INCLUDE_DIR}")
    message(STATUS "LLVM GTest libraries found - unit tests enabled")
    set(PREDICATES_GTEST_AVAILABLE TRUE)
else()
    message(STATUS "LLVM GTest libraries not found - unit tests disabled")
    set(PREDICATES_GTEST_AVAILABLE FALSE)
endif()

# Check if LLVM has Z3 support
if(LLVM_WITH_Z3)
    list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
    find_package(Z3 REQUIRED MODULE)
    add_definitions(-DHAVE_Z3)
    message(STATUS "Z3 features enabled with mathematical integer arithmetic")
else()
    message(STATUS "LLVM built without Z3 support - Z3 features disabled")
endif()

add_definitions(${LLVM_DEFINITIONS})

include(CTest)

add_subdirectory(src)
if(BUILD_TESTING)
    add_subdirectory(test)
endif()
