# 「J2」单元测试
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

# Qt 可用时验证真实控件的布局、手势与标注结果；无桌面时也可离屏运行。
if(TARGET Qt6::Widgets OR TARGET Qt5::Widgets)
    set(JIE2_TEST_QT_FOUND OFF)
    if(TARGET Qt6::Widgets)
        find_package(Qt6 QUIET COMPONENTS Test)
        if(TARGET Qt6::Test)
            set(JIE2_TEST_QT_FOUND ON)
            set(JIE2_TEST_QT_LIBS Qt6::Widgets Qt6::Test Qt6::Network)
        endif()
    elseif(TARGET Qt5::Widgets)
        find_package(Qt5 QUIET COMPONENTS Test)
        if(TARGET Qt5::Test)
            set(JIE2_TEST_QT_FOUND ON)
            set(JIE2_TEST_QT_LIBS Qt5::Widgets Qt5::Test Qt5::Network)
        endif()
    endif()

    if(JIE2_TEST_QT_FOUND)
        add_executable(jie2_ui_alignment_tests
            ui_alignment_tests.cpp
            ../assets/brand/brand.qrc
            ../src/ui/app_paths.cpp
            ../src/ui/selection_overlay.cpp
            ../src/ui/annotation_render.cpp
            ../src/ui/clothesline.cpp
            ../src/ui/flight_overlay.cpp
            ../src/ui/pin_window.cpp
            ../src/ui/tray.cpp
            ../src/ui/theme.cpp
            ../src/ui/ocr_runner.cpp
            ../src/ui/settings_dialog.cpp
            ../src/ui/single_instance.cpp
        )
        target_include_directories(jie2_ui_alignment_tests PRIVATE ../src)
        target_link_libraries(jie2_ui_alignment_tests PRIVATE
            jie2_core jie2_platform ${JIE2_TEST_QT_LIBS})
        add_test(NAME ui_alignment_tests COMMAND jie2_ui_alignment_tests)
        set_tests_properties(ui_alignment_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME windows_dependencies_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/windows_dependencies_tests.py")
endif()
