# 「截2」UI 层（ui）
#
# 依赖方向：ui -> platform -> core（允许）；ui 不得被下层反向依赖。
# 本层是唯一允许使用 Qt 的地方。替换 UI 技术（Widgets <-> QML）
# 只应影响本目录，不应波及 core 与 platform。

add_executable(jie2 WIN32 MACOSX_BUNDLE
    annotation_render.cpp
    clothesline.cpp
    main.cpp
    ocr_runner.cpp
    pin_window.cpp
    tray.cpp
    selection_overlay.cpp
)

target_include_directories(jie2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(jie2 PRIVATE
    jie2_platform
    jie2_core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
)



if(APPLE)
    set_target_properties(jie2 PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "截2"
        MACOSX_BUNDLE_GUI_IDENTIFIER "app.jie2.desktop"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    )
endif()

if(MSVC)
    target_compile_options(jie2 PRIVATE /W4)
else()
    target_compile_options(jie2 PRIVATE -Wall -Wextra)
endif()
