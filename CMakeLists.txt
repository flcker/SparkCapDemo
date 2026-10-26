cmake_minimum_required(VERSION 3.24)

project(SparkCapDemo
    VERSION 1.0.0
    DESCRIPTION "SparkCap consumer demo: camera live preview"
    LANGUAGES C CXX)

if (MSVC AND MSVC_VERSION LESS 1930)
    message(FATAL_ERROR "SparkCapDemo requires Visual Studio 2022 (MSVC >= 14.30).")
endif()

# ---- 应用（内置 SDK：sdk/include + sdk/lib + sdk/bin）----
add_executable(sparkcap_demo WIN32 src/main.cpp)

target_include_directories(sparkcap_demo PRIVATE sdk/include)
target_link_directories(sparkcap_demo PRIVATE sdk/lib)
target_link_libraries(sparkcap_demo PRIVATE sparkcap user32 gdi32 gdiplus shlwapi)

target_compile_options(sparkcap_demo PRIVATE /W4 /utf-8)

# 运行时 DLL 就位（sdk/bin → 输出目录）
add_custom_command(TARGET sparkcap_demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/sdk/bin/sparkcap.dll $<TARGET_FILE_DIR:sparkcap_demo>)

# 设备探针（诊断工具）
add_executable(probe tools/probe.cpp)
target_include_directories(probe PRIVATE sdk/include)
target_link_directories(probe PRIVATE sdk/lib)
target_link_libraries(probe PRIVATE sparkcap)
target_compile_options(probe PRIVATE /W4 /utf-8)

# stride 诊断（J.ZAO 高分辨率马赛克排查）
add_executable(stride_diag tools/stride_diag.cpp)
target_include_directories(stride_diag PRIVATE sdk/include)
target_link_directories(stride_diag PRIVATE sdk/lib)
target_link_libraries(stride_diag PRIVATE sparkcap)
target_compile_options(stride_diag PRIVATE /W4 /utf-8)
