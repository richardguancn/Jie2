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

# ---------------------------------------------------------------------------
# jie2-line —— Wayland layer-shell 顶部条（衣架渲染进程）
#
# 仅 Linux 构建；需要 wayland-client 与 wayland-scanner。
# 协议代码在构建时由 wayland-scanner 从仓库内的 wlr-protocols XML 生成
# （XML 是唯一不能由包管理器提供的东西 —— Debian 的 wlr-protocols 包
# 并不存在，所以随仓库携带，来源与许可见文件头部注释）。
# ---------------------------------------------------------------------------
if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(WAYLAND_CLIENT QUIET wayland-client)
        # wayland-protocols 只提供 XML（xdg-shell 等）；wlr-layer-shell 的
        # get_popup 请求引用 xdg_popup 类型，链接时需要 xdg-shell 的
        # interface 符号 —— 因此这里也要生成并编译它。
        pkg_check_modules(WAYLAND_PROTOCOLS QUIET wayland-protocols)
        if(WAYLAND_PROTOCOLS_FOUND)
            pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
        endif()
    endif()
    find_program(WAYLAND_SCANNER wayland-scanner)

    if(WAYLAND_CLIENT_FOUND AND WAYLAND_SCANNER)
        set(WS_PROTOCOL_XML
            ${CMAKE_SOURCE_DIR}/third_party/wlr-protocols/wlr-layer-shell-unstable-v1.xml)
        set(WS_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/wayland-gen)
        file(MAKE_DIRECTORY ${WS_GEN_DIR})
        add_custom_command(
            OUTPUT
                ${WS_GEN_DIR}/wlr-layer-shell-unstable-v1-client-protocol.h
                ${WS_GEN_DIR}/wlr-layer-shell-unstable-v1-protocol.c
            COMMAND ${WAYLAND_SCANNER} client-header
                ${WS_PROTOCOL_XML}
                ${WS_GEN_DIR}/wlr-layer-shell-unstable-v1-client-protocol.h
            COMMAND ${WAYLAND_SCANNER} private-code
                ${WS_PROTOCOL_XML}
                ${WS_GEN_DIR}/wlr-layer-shell-unstable-v1-protocol.c
            # wayland-scanner 1.21 生成的 C 头里，get_layer_surface 的参数
            # 名恰好是 `namespace` —— 在 C 里合法，在 C++ 里是关键字，
            # 直接编译会报 "expected ',' or '...' before 'namespace'"。
            # 上游到 1.22+ 才处理这个保留字问题；这里对生成头做一处最小
            # 后处理（只替换作为标识符出现的位置，不动注释）。
            COMMAND sed -i -e "s/namespace)/namespace_)/g"
                        -e "s/namespace,/namespace_/g"
                ${WS_GEN_DIR}/wlr-layer-shell-unstable-v1-client-protocol.h
            DEPENDS ${WS_PROTOCOL_XML}
            COMMENT "生成 wlr-layer-shell 协议代码（含 C++ 关键字后处理）"
            VERBATIM
        )

        # xdg-shell（依赖协议）：wlr-layer-shell 的 get_popup 引用 xdg_popup。
        set(XDG_SHELL_XML "")
        if(WAYLAND_PROTOCOLS_FOUND AND EXISTS
           "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
            set(XDG_SHELL_XML
                "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
            add_custom_command(
                OUTPUT
                    ${WS_GEN_DIR}/xdg-shell-client-protocol.h
                    ${WS_GEN_DIR}/xdg-shell-protocol.c
                COMMAND ${WAYLAND_SCANNER} client-header
                    ${XDG_SHELL_XML} ${WS_GEN_DIR}/xdg-shell-client-protocol.h
                COMMAND ${WAYLAND_SCANNER} private-code
                    ${XDG_SHELL_XML} ${WS_GEN_DIR}/xdg-shell-protocol.c
                DEPENDS ${XDG_SHELL_XML}
                COMMENT "生成 xdg-shell 协议代码（wlr-layer-shell 的依赖）"
                VERBATIM
            )
        endif()

        set(JIE2_LINE_SOURCES
            jie2_line.cpp
            ${WS_GEN_DIR}/wlr-layer-shell-unstable-v1-protocol.c
        )
        if(NOT XDG_SHELL_XML STREQUAL "")
            list(APPEND JIE2_LINE_SOURCES ${WS_GEN_DIR}/xdg-shell-protocol.c)
        endif()

        add_executable(jie2-line ${JIE2_LINE_SOURCES})
        target_include_directories(jie2-line PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${WS_GEN_DIR}
            ${WAYLAND_CLIENT_INCLUDE_DIRS}
        )
        target_link_directories(jie2-line PRIVATE ${WAYLAND_CLIENT_LIBRARY_DIRS})
        target_link_libraries(jie2-line PRIVATE
            jie2_platform
            jie2_core
            ${WAYLAND_CLIENT_LIBRARIES}
        )
        target_compile_options(jie2-line PRIVATE ${WAYLAND_CLIENT_CFLAGS_OTHER})
        if(NOT MSVC)
            target_compile_options(jie2-line PRIVATE -Wall -Wextra)
        endif()
        message(STATUS "jie2: wayland-client found — jie2-line (layer-shell) enabled")
    else()
        message(STATUS "jie2: wayland-client or wayland-scanner missing — jie2-line disabled")
    endif()
endif()
