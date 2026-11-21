cmake_minimum_required(VERSION 3.16)
project(POTG_League CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# 1. Automatically downloads and compiles CPR
FetchContent_Declare(
    cpr
    GIT_REPOSITORY https://github.com/libcpr/cpr.git
    GIT_TAG 1.10.5
)
FetchContent_MakeAvailable(cpr)

# 2. Automatically downloads nlohmann/json
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

# 3. Builds your main.cpp into main.exe
add_executable(main main.cpp potg_video.cpp)
target_link_libraries(main PRIVATE cpr::cpr nlohmann_json::nlohmann_json winmm)
target_include_directories(main PRIVATE third_party)
# Fonts, the Blender script and the model cache live in intro/
target_compile_definitions(main PRIVATE POTG_ASSETS="${CMAKE_SOURCE_DIR}/intro")
# Runs in the background with no console window
set_target_properties(main PROPERTIES WIN32_EXECUTABLE TRUE)
if(MSVC)
    target_compile_options(main PRIVATE /utf-8 /bigobj)
    target_link_options(main PRIVATE /ENTRY:mainCRTStartup)
endif()

# 4. Copies the DLLs next to main.exe after each build
add_custom_command(TARGET main POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:main> $<TARGET_FILE_DIR:main>
    COMMAND_EXPAND_LISTS)