# 「截2」图像内核（core）
#
# 硬约束：本目标只允许依赖 C++ 标准库。
# 不得链接 Qt、不得包含任何平台头文件 —— 这是三层解耦的底线，
# 也是"将来换 UI 层不动内核"与"内核可脱离 GUI 单测"的前提。

add_library(jie2_core STATIC
    annotation.cpp
    capability.cpp
    capture_history.cpp
    config.cpp
    image_buffer.cpp
    image_ops.cpp
    ocr.cpp
    snap.cpp
)

target_include_directories(jie2_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_compile_features(jie2_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(jie2_core PRIVATE /W4)
else()
    target_compile_options(jie2_core PRIVATE -Wall -Wextra)
endif()
