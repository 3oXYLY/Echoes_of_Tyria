cmake_minimum_required(VERSION 3.20)
project(LyricHudTest CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(LYRICHUD_REPO "" CACHE STRING "GitHub repository link used by Nexus to check for updates")

set(CORE_SOURCES
    src/Main.cpp
    src/Media.cpp
    src/Lyrics.cpp
    src/Depth.cpp
    src/Occlusion.cpp
    src/Cover.cpp
    src/Audio.cpp
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
)

add_library(LyricHudCore SHARED ${CORE_SOURCES})

add_library(LyricHud SHARED ${CORE_SOURCES} res/fonts.rc)
target_compile_definitions(LyricHud PRIVATE LYRICHUD_RELEASE)
set_target_properties(LyricHud PROPERTIES OUTPUT_NAME EchoesOfTyria)
if(LYRICHUD_REPO)
    target_compile_definitions(LyricHud PRIVATE LYRICHUD_REPO="${LYRICHUD_REPO}")
endif()

add_library(LyricHudTest SHARED
    src/Loader.cpp
)

foreach(target LyricHudCore LyricHud LyricHudTest)
    target_include_directories(${target} PRIVATE include imgui)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    set_target_properties(${target} PROPERTIES PREFIX "")
    if(MSVC)
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/permissive- /utf-8 /bigobj>)
    else()
        target_link_options(${target} PRIVATE -static)
    endif()
endforeach()

foreach(target LyricHudCore LyricHud)
    if(MSVC)
        target_link_libraries(${target} PRIVATE windowsapp winhttp mmdevapi ole32)
    else()
        target_link_libraries(${target} PRIVATE winhttp mmdevapi ole32)
    endif()
endforeach()

set(GW2_ADDONS_DIR "" CACHE PATH "Guild Wars 2 addons folder for automatic hot-reload copy")
if(GW2_ADDONS_DIR)
    add_custom_command(TARGET LyricHudCore POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GW2_ADDONS_DIR}/LyricHud/core"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:LyricHudCore>
            "${GW2_ADDONS_DIR}/LyricHud/core/LyricHudCore.dll")
    add_custom_command(TARGET LyricHudTest POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:LyricHudTest>
            "${GW2_ADDONS_DIR}/$<TARGET_FILE_NAME:LyricHudTest>.update")
endif()
