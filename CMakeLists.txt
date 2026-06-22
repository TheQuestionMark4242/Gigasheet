cmake_minimum_required(VERSION 3.20)
project(Spreadsheet LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED)

find_program(WX_CONFIG_EXECUTABLE wx-config HINTS /mingw64/bin)
if(WX_CONFIG_EXECUTABLE)
    message(STATUS "Found wx-config: ${WX_CONFIG_EXECUTABLE}")

    execute_process(COMMAND ${WX_CONFIG_EXECUTABLE} --cxxflags
                    OUTPUT_VARIABLE WX_CXX_FLAGS
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${WX_CONFIG_EXECUTABLE} --libs core,base
                    OUTPUT_VARIABLE WX_LIBS
                    OUTPUT_STRIP_TRAILING_WHITESPACE)

    separate_arguments(WX_CXX_FLAGS)
    separate_arguments(WX_LIBS)
else()
    message(FATAL_ERROR "wx-config not found — please install wxWidgets (pacman -S mingw-w64-x86_64-wxWidgets).")
endif()
message(STATUS "Generator = ${CMAKE_GENERATOR}")
message(STATUS "WX_CXX_FLAGS = ${WX_CXX_FLAGS}")
add_executable(
    spreadsheet 
    src/app.cpp
    src/ui/app.cpp
    src/ui/spreadsheet_frame.cpp
    src/model/mmapped_table.cpp 
    src/storage/csv_importer.cpp
    src/storage/memory_usage.cpp
)

target_include_directories(
    spreadsheet
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party
)

target_compile_options(
    spreadsheet
    PRIVATE
    ${WX_CXX_FLAGS}
)

target_link_libraries(
    spreadsheet
    PRIVATE
    ${WX_LIBS}
)

if(WIN32)
    target_link_libraries(
        spreadsheet
        PRIVATE
        psapi
    )
endif()

# ----------------------------------------------------------------------------- 
# Default to Release build if unspecified
# -----------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
endif()
