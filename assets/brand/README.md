# J2图标

以用户手绘草图为依据：下方是大写 J，上方是一片独立叶子。J 的顶端保留草图的浅弧，叶子向右上舒展。应用图标采用浅草绿渐变与柔白标志；Mac 菜单栏采用单色标志与透明背景，颜色跟随菜单栏背景自动调整。

参考 [Apple 应用图标设计指引](https://developer.apple.com/design/human-interface-guidelines/app-icons) 的简洁轮廓、视觉留白和小尺寸辨识原则。本次提供传统 macOS 应用图标文件，未制作 Icon Composer 分层图标。

- `app-icon.svg`：可编辑的完整图标原稿。
- `mark.svg`：透明底、白色标志原稿。
- `menubar-white.svg`：针对菜单栏收紧留白的白色标志。
- `menubar-black.svg`：同一轮廓的深色标志，供 macOS 单色模板使用。
- `app/`：16 至 1024 像素的完整图标。
- `menubar/`：白色与深色各 22、44、66 像素的菜单栏图标，覆盖常规和高分辨率显示。
- `AppIcon.icns`：Mac 应用图标。
- `preview.png`：应用图标、菜单栏和小尺寸效果预览。
- `brand.qrc`：程序使用的图片资源。

macOS 使用深色标志作为系统模板，浅色菜单栏显示深色，深色菜单栏显示浅色。应用只常驻菜单栏，不在 Dock 中显示；可从菜单栏打开设置或退出。

修改原稿后，用 `rsvg-convert -w 尺寸 -h 尺寸 原稿.svg -o 输出.png` 重新导出对应尺寸；使用 `iconutil` 将完整 iconset 合成为 `AppIcon.icns`。打包脚本默认复制已导出的常用尺寸，不增加安装依赖。
