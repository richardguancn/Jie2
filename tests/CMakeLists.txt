# 「截2」单元测试
#
# 只测 core（图像内核）：它不依赖 Qt 与平台 API，因此测试可在任何环境运行。
# 平台层的行为验证走 src/tools 下的 jie2-probe。

add_executable(jie2_core_tests
    core_tests.cpp
)

target_link_libraries(jie2_core_tests PRIVATE jie2_core)

if(MSVC)
    target_compile_options(jie2_core_tests PRIVATE /W4)
else()
    target_compile_options(jie2_core_tests PRIVATE -Wall -Wextra)
endif()

add_test(NAME core_tests COMMAND jie2_core_tests)
