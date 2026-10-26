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
            ../src/ui/system_menu.cpp
        )
        target_include_directories(jie2_ui_alignment_tests PRIVATE ../src)
        target_link_libraries(jie2_ui_alignment_tests PRIVATE
            jie2_core jie2_platform ${JIE2_TEST_QT_LIBS})
        if(APPLE)
            target_sources(jie2_ui_alignment_tests PRIVATE native_menu_test_support.mm)
            add_executable(jie2_native_snap_fixture MACOSX_BUNDLE native_snap_fixture.mm)
            set_target_properties(jie2_native_snap_fixture PROPERTIES
                MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/native_snap_fixture.plist.in")
            target_link_libraries(jie2_native_snap_fixture PRIVATE "-framework Cocoa" "-framework ApplicationServices")
        endif()
        # Qt 5.12 的 offscreen 插件不支持托盘，初始化会崩溃；用虚拟 X11
        # 运行同一套真实控件测试，覆盖与麒麟 X11 桌面相同的平台插件。
        if(UNIX AND NOT APPLE AND JIE2_QT_VERSION_MAJOR EQUAL 5)
            find_program(JIE2_XVFB_RUN xvfb-run)
        endif()
        if(JIE2_XVFB_RUN AND JIE2_QT_VERSION_MAJOR EQUAL 5)
            add_test(NAME ui_alignment_tests
                COMMAND "${JIE2_XVFB_RUN}" -a -s "-screen 0 1280x1024x24" $<TARGET_FILE:jie2_ui_alignment_tests>)
            set_tests_properties(ui_alignment_tests PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=xcb;XDG_SESSION_TYPE=x11")
        else()
            add_test(NAME ui_alignment_tests COMMAND jie2_ui_alignment_tests)
            set_tests_properties(ui_alignment_tests PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        endif()
    endif()
endif()

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME windows_dependencies_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/windows_dependencies_tests.py")
endif()
