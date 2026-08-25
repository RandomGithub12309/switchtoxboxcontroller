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
