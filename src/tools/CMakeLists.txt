# 「截2」CLI 工具
#
# 不链接 Qt：这些工具只依赖 core 与 platform，可在无桌面会话、
# 甚至无 Qt 的环境构建运行 —— 用于 CI 与现场诊断。

add_executable(jie2-probe
    probe_main.cpp
)

target_include_directories(jie2-probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(jie2-probe PRIVATE
    jie2_platform
    jie2_core
)

if(MSVC)
    target_compile_options(jie2-probe PRIVATE /W4)
else()
    target_compile_options(jie2-probe PRIVATE -Wall -Wextra)
endif()
