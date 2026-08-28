cmake_minimum_required(VERSION 3.16)
project(SwitchProXInput CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3)
else()
    add_compile_options(-Wall -Wextra)
endif()

add_executable(SwitchProXInput WIN32
    main.cpp
    config.cpp
    engine.cpp
    gui.cpp
    app.rc
)

target_compile_definitions(SwitchProXInput PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
)

target_link_libraries(SwitchProXInput PRIVATE setupapi hid shell32 advapi32)

if(MINGW)
    # Self-contained binary: no libgcc/libstdc++ DLLs needed next to the exe.
    target_link_options(SwitchProXInput PRIVATE -static)
endif()

# Unit tests for the pure-logic parts (parser + drift correction). They have
# no Windows dependencies and run on any platform.
option(SXPX_BUILD_TESTS "Build unit tests (parse_test, drift_test)" OFF)
if(SXPX_BUILD_TESTS)
    enable_testing()
    add_executable(parse_test tests/parse_test.cpp)
    add_executable(drift_test tests/drift_test.cpp)
    add_executable(pipeline_test tests/pipeline_test.cpp)
    target_include_directories(parse_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_include_directories(drift_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_include_directories(pipeline_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME parse_test COMMAND parse_test)
    add_test(NAME drift_test COMMAND drift_test)
    add_test(NAME pipeline_test COMMAND pipeline_test)
endif()
