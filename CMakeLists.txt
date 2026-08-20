cmake_minimum_required(VERSION 3.14)
project(playbacq CXX)

cmake_policy(SET CMP0167 NEW)

include(CheckIncludeFileCXX)

check_include_file_cxx(any HAS_ANY)
check_include_file_cxx(string_view HAS_STRING_VIEW)

# Enable coroutines support for the check
set(CMAKE_CXX_FLAGS_SAVE "${CMAKE_CXX_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fcoroutines")
check_include_file_cxx(coroutine HAS_COROUTINE)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS_SAVE}")
if(NOT "${CMAKE_CXX_STANDARD}" STREQUAL "")
    # Do nothing
elseif(HAS_ANY AND HAS_STRING_VIEW AND HAS_COROUTINE)
    set(CMAKE_CXX_STANDARD 23)
elseif(HAS_ANY AND HAS_STRING_VIEW)
    set(CMAKE_CXX_STANDARD 17)
else()
    set(CMAKE_CXX_STANDARD 14)
endif()

if(CMAKE_CXX_STANDARD LESS 17)
    message(FATAL_ERROR "c++17 or higher is required")
elseif(CMAKE_CXX_STANDARD LESS 23)
    message(STATUS "use c++17")
else()
    message(STATUS "use c++23")
endif()

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Drogon CONFIG REQUIRED)
find_package(AWSSDK REQUIRED COMPONENTS s3)

option(BUILD_BACKEND "Build the backend executable" ON)
option(BUILD_WORKER "Build the worker executable" ON)
option(BUILD_LOCAL_DEV "Build for local development with MinIO" OFF)
option(USE_LOCAL_WORKER "Use local worker for encoding" OFF)

option(ENABLE_COVERAGE "Enable coverage reporting" OFF)
if(ENABLE_COVERAGE)
    add_compile_options(-O0 -g --coverage)
    add_link_options(--coverage)
    message(STATUS "Build Mode: COVERAGE (Instrumentation enabled)")
endif()

if(BUILD_WORKER)
    # for FFmpeg worker
    add_executable(playbacq_worker worker/worker.cpp)
    set(Boost_USE_STATIC_LIBS ON)
    find_package(Boost REQUIRED COMPONENTS system filesystem atomic)
    find_package(Boost REQUIRED COMPONENTS system filesystem)
    find_package(CURL REQUIRED)
    target_link_libraries(playbacq_worker PRIVATE
        Boost::system
        Boost::filesystem
        hiredis
        redis++
        hiredis_ssl
        ${AWSSDK_LINK_LIBRARIES}
        zstd
        CURL::libcurl
    )
    target_include_directories(playbacq_worker PRIVATE ${Boost_INCLUDE_DIRS})

    if(BUILD_LOCAL_DEV)
        target_compile_definitions(playbacq_worker PRIVATE USE_INTERNAL_S3)
        message(STATUS "Worker Encoder: CPU (libx264)")
    else()
        message(STATUS "Worker Encoder: GPU (h264_nvenc)")
    endif()
    message(STATUS "Configured to build: WORKER")
endif()

if(BUILD_BACKEND)
    file(GLOB SRC_DIR *.cpp)
    list(REMOVE_ITEM SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")

    aux_source_directory(controllers CTL_SRC)
    aux_source_directory(filters FILTER_SRC)
    aux_source_directory(plugins PLUGIN_SRC)
    aux_source_directory(models MODEL_SRC)

    # コアライブラリの作成
    add_library(playbacq_core OBJECT
        ${SRC_DIR}
        ${CTL_SRC}
        ${FILTER_SRC}
        ${PLUGIN_SRC}
        ${MODEL_SRC}
    )

    target_link_libraries(playbacq_core PUBLIC
        Drogon::Drogon
        ${AWSSDK_LINK_LIBRARIES}
        zstd
        redis++
        hiredis_ssl
    )

    drogon_create_views(playbacq_core ${CMAKE_CURRENT_SOURCE_DIR}/views
        ${CMAKE_CURRENT_BINARY_DIR})

    target_include_directories(playbacq_core PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/models
        ${CMAKE_CURRENT_SOURCE_DIR}/plugins
    )

    if(USE_LOCAL_WORKER)
        target_compile_definitions(playbacq_core PRIVATE USE_LOCAL_WORKER)
        message(STATUS "Worker Dispatch: LOCAL")
    endif()

    add_executable(${PROJECT_NAME} main.cpp)
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -Wpedantic -O3
    )
    target_link_libraries(${PROJECT_NAME} PRIVATE playbacq_core)

    if(BUILD_LOCAL_DEV)
        target_compile_definitions(playbacq_core PUBLIC USE_INTERNAL_S3)
        target_compile_definitions(${PROJECT_NAME} PRIVATE USE_INTERNAL_S3)
    endif()
    message(STATUS "Configured to build: BACKEND")
endif()


# Enable testing
enable_testing()

# Add tests if BUILD_TESTING is enabled
if(BUILD_TESTING AND BUILD_BACKEND)
    add_subdirectory(test)
endif()

if(BUILD_LOCAL_DEV)
    message(STATUS "Build Mode: LOCAL_DEV (MinIO enabled)")
else()
    message(STATUS "Build Mode: PRODUCTION (MinIO disabled)")
endif()
