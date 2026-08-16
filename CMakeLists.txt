cmake_minimum_required(VERSION 3.20)
project(Spreadsheet LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED)
find_package(FLEX REQUIRED)
find_package(BISON REQUIRED)

BISON_TARGET(
    ExprParser
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/expr.y
    ${CMAKE_CURRENT_BINARY_DIR}/expr_parser.cpp
    DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/expr_parser.hpp
)
FLEX_TARGET(
    ExprLexer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser/expr.l
    ${CMAKE_CURRENT_BINARY_DIR}/expr_lexer.cpp
)
ADD_FLEX_BISON_DEPENDENCY(ExprLexer ExprParser)

find_program(WX_CONFIG_EXECUTABLE wx-config HINTS /mingw64/bin)
if(WX_CONFIG_EXECUTABLE)
    message(STATUS "Found wx-config: ${WX_CONFIG_EXECUTABLE}")
    execute_process(COMMAND bash -lc "wx-config --cxxflags"
                    OUTPUT_VARIABLE WX_CXX_FLAGS
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND bash -lc "wx-config --libs core,base"
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
    src/ui/statistics_dialog.cpp
    src/ui/formula_autocomplete.cpp
    src/model/mmapped_table.cpp 
    src/storage/csv_importer.cpp
    src/storage/memory_usage.cpp
    src/stats/stats_writer.cpp
    src/stats/stats_reader.cpp
    src/stats/stats_query.cpp
    src/parser/expr_eval.cpp
    ${BISON_ExprParser_OUTPUTS}
    ${FLEX_ExprLexer_OUTPUTS}
)

# Copy the application icon next to the executable so the app can load it at
# runtime (window + taskbar icon).
add_custom_command(TARGET spreadsheet POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/appicon.png
        $<TARGET_FILE_DIR:spreadsheet>/appicon.png)

target_include_directories(
    spreadsheet
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser
    ${CMAKE_CURRENT_BINARY_DIR}
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

# Windows-only target: psapi (memory usage) and dwmapi (dark title bar).
# Linked unconditionally because the MSYS "Unix Makefiles" generator does not
# set WIN32, which would otherwise skip these.
target_link_libraries(
    spreadsheet
    PRIVATE
    psapi
    dwmapi
)

# ----------------------------------------------------------------------------- 
# Default to Release build if unspecified
# -----------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
endif()
