# 2026-10-17 功能核对与本次修复

核对基线：仓库《截2-可行性评估报告》1.8 原有清单，以及本次用户提出的设置、大模型识别、翻译和 Windows 成品启动问题。不是对 Snipaste 最新版本的新增功能作承诺。

“已有实现”表示找到了对应实现，不等于所有系统的完整交互已验收。本次实际运行范围见文末。替代操作、命令行参数不再直接算作图形界面功能完成；多个显示器组成的坐标空间也不能当作 Windows 虚拟桌面支持。

## 本次修复

- 托盘新增“设置…”：常规、截图与标注、贴图、快捷键、文字识别、翻译六页。服务地址、模型、密钥和等待时间可配置；识别和翻译可分别使用不同服务。保存后立即应用，取消不改变设置。
- 常规文字识别统一使用支持图片输入的大模型；旧 auto 设置也走大模型。界面不再默认使用系统 OCR 或 tesseract。旧的显式本地命令行验证入口保留兼容。
- 翻译剪贴板文字或图片，目标语言可配置。发送内容前显示服务地址并确认；失败不覆盖剪贴板，成功复制译文并显示结果。
- 保存设置时合并已有内容，修复密钥、自定义命令、快捷键等被重写丢失的问题；使用临时文件提交，限制设置文件为当前用户读写。密钥在文件内是明文，界面明确提醒不要分享设置文件。
- “平台能力”改为中文状态说明；可用、不适用和受限分开呈现，内部名称放到详细信息中。用户给出的内容表示能力可用，不构成程序失败的证据。
- Windows 打包递归检查主程序、全部 DLL 和插件的依赖，补齐同一工具链目录内的非系统库，再独立核对。原脚本只查看两个 exe，未检查 Qt 等库的下一级依赖，足以漏掉运行库；尚未获得用户现场缺失 DLL 的文件名，不能断言具体缺的是哪一个。
- Windows 发布流程增加解压成品后启动检查，搜索路径去掉开发用 Qt/MinGW，检查窗口支持、PNG 与 HTTPS；检查失败阻止上传成品。当前环境无 Windows，未实际执行这项 Windows 检查。

## 原有专业版清单逐项核对

| 原有条目 | 当前结论与缺口 | 对应位置 |
|---|---|---|
| 超级截图（跨屏拼接）及历史 | **未完整实现**：单屏截图有历史，跨屏单张拼接未实现 | `tray.cpp` 截图路径、`capture_history.cpp` |
| 超级截图绑定截图、复制并贴图 | **部分**：普通截图已有贴图按钮；超级截图未实现 | `selection_overlay.cpp`、`tray.cpp::deliverSelection` |
| F5 刷新并保留标注、长按刷新 | **部分**：同尺寸刷新保留标注；屏幕几何改变会清空；长按依赖系统重复按键，没有独立持续刷新控制 | `selection_overlay.cpp::recapture` / F5 分支 |
| 自定义截图尺寸预设 | **未实现**：固定区域参数不能代替预设管理器 | `main.cpp` select-rect |
| 圆角截图及大半径 | **已有实现**；本次补设置入口 | `image_ops.cpp`、设置窗口 |
| 边框颜色和宽度 | **已有实现**；本次补设置入口 | `tray.cpp::decorateForOutput` |
| 阴影颜色和宽度 | **部分**：能开关，颜色、宽度仍固定 | `tray.cpp::decorateForOutput` |
| 区域单位 dp/px | **部分**：显示像素，不能切换 | `selection_overlay.cpp` |
| 记住锁定纵横比 | **未实现** | 选区拖动路径无比例持久化 |
| 批量保存选中贴图 | **部分**：现有菜单保存全部贴图，没有“只保存选中项” | `tray.cpp::saveAllPins` |
| 触发角开关热键、程序黑名单 | **部分**：有角落、停留时间和开关；没有开关热键与程序黑名单 | `tray.cpp::cornerTick`、设置窗口 |
| 保存时隐藏截图/贴图窗口 | **未完整实现**：截图有保存对话框，未实现统一隐藏贴图流程；不能再写“不适用” | `tray.cpp::deliverSelection` |
| 序号、多组计数、起始数字、加减序号 | **部分**：有单组序号和数字偏移，没有多组独立计数 | `selection_overlay.cpp::nextNumber`、`annotation.cpp` |
| 放大镜标注 | **已有实现** | `selection_overlay.cpp`、`annotation_render.cpp` |
| 圆角矩形标注 | **已有实现** | 同上 |
| 马克笔/马赛克/模糊/橡皮擦椭圆形状 | **已有实现，未在本轮逐工具验收** | `annotation_render.cpp`、椭圆开关 |
| 区域橡皮擦旋转 | **已有实现，未在本轮专门验收** | `selection_overlay.cpp` 选中变换、`annotation_render.cpp` |
| 双向、反向箭头 | **已有实现** | `annotation_render.cpp`、快捷键映射 |
| 虚线、点线 | **部分**：只有虚线开关，点线未单列 | `annotation_render.cpp` DashLine |
| Shift+滚轮缩放标注 | **已有实现，未在本轮专门验收** | `selection_overlay.cpp::wheelEvent` |
| 中心点、等比缩放 | **已有实现，未在本轮专门验收** | `selection_overlay.cpp` 选中变换 |
| 自由选中、二次编辑、橡皮擦禁用二次编辑 | **已有实现**；本次补橡皮擦设置 | `selection_overlay.cpp`、设置窗口 |
| 文字描边颜色/粗细、背景填充 | **部分**：有描边渲染，背景填充与完整的文字样式设置未实现 | `annotation_render.cpp` 文字分支 |
| 调色板数量自定义 | **已有实现**；设置范围与实际支持的 2–10 一致 | `selection_overlay.cpp`、设置窗口 |
| 不同形状记住颜色 | **已有实现，未在本轮专门验收** | `selection_overlay.cpp` 颜色记忆 |
| 贴图开关显示标注 | **未实现**：保存后已合成图片，没有独立标注层 | `tray.cpp::deliverSelection` |
| Windows 虚拟桌面 | **未实现**：显示器坐标与虚拟桌面是不同能力；未找到切换桌面/固定到全部桌面的集成 | `windows_platform.cpp`、`pin_window.cpp` |
| 贴图反色 | **已有实现，本轮原有测试覆盖** | `pin_window.cpp`、`ui_alignment_tests.cpp` |
| 自定义背景色复制/保存保留 | **部分**：白板背景可保留；贴图独立背景色配置未提供 | `main.cpp` whiteboard、`pin_window.cpp` |
| 缩放后按原尺寸保存 | **已有实现，本轮原有测试覆盖图像变换** | `pin_window.cpp::saveTo` |
| Ctrl+A 全选贴图 | **已有实现** | `tray.cpp::toggleSelectAllPins` |
| Esc / Shift+Esc 自定义 | **已有实现**；本次补设置入口，应用于已打开贴图 | `pin_window.h::setDismissActions` |
| 透明白板的鼠标穿透 | **部分**：贴图支持穿透，一次性白板不是可切换穿透的常驻窗口 | `pin_window.cpp`、白板路径 |
| 缩略图快捷键自定义 | **部分**：可选 Shift/Ctrl/Alt+双击，不支持任意按键组合 | `pin_window.h::setThumbnailModifier`、设置窗口 |
| 文字识别 | **本次修复**：默认大模型、界面可配置 | `ocr_runner.cpp`、设置窗口、托盘入口 |
| 腾讯云 / OCR.space 专用接口 | **未实现专用接口**：本产品按本次要求使用兼容 OpenAI 的大模型服务 | `ocr_runner.cpp` |
| 二维码/条形码识别 | **有条件可用**：需要外部 zbarimg；安装包未内置，不能宣称开箱即用 | `ocr_runner.cpp::scanBarcode` |
| 深色二维码增强 | **已有反色重试实现，仍依赖 zbarimg** | 同上 |
| 部分应用内快捷键自定义 | **已有实现**；本次补界面编辑与冲突检查，不包含全局快捷键 | `shortcut_map.cpp`、设置窗口 |
| Ctrl+Alt+C 与文件命名规则 | **部分**：有快捷保存，文件名按固定时间格式生成，不能自定义规则 | `tray.cpp::captureFileName`、热键路径 |
| 自定义命令、超长命令显示 | **未实现完整功能**：旧 OCR 命令不是通用命令管理 | `ocr_runner.cpp` 旧兼容入口 |
| 跟随系统主题 | **部分**：能按系统当前状态解析；未确认系统主题切换时所有窗口自动同步 | `theme.cpp`、`tray.cpp::loadConfig` |
| Windows 10 原生分享 | **未实现**，对 Windows 不能标作不适用 | `selection_overlay.cpp` 无分享入口 |
| 高级设置三项 | **未提供同名设置**：没有完整对应实现，不算完成 | `config` 与设置窗口 |

## 命令行选项逐项

以下仅确认参数解析和调用路径存在；本轮没有重新执行 Linux/Windows 的全部命令行验证脚本。参数存在不代表对应图形界面已完成。

| 基线选项 | 当前情况 |
|---|---|
| `--hold` | 有解析与交互截图路径 |
| `--block [SECONDS]` | 接受参数但忽略秒数；命令自身同步执行，不提供按秒等待上限 |
| `-o silent` | 需要特别区分：实际 `-o` 是输出文件路径，字面 `silent` 不是完整兼容的输出模式 |
| `-o no-auto-save` | 不是同名兼容语义；实际使用 `--no-save` |
| `--size WIDTH HEIGHT` | 有实现 |
| `--keep_aspect_ratio` | 实际使用 `--keep-aspect-ratio`，不支持下划线拼写 |
| `--shadow / --no-shadow` | 有实现；阴影样式参数固定 |
| `--active-screen` | 有实现，仅选一块屏幕 |
| `barcode-scan` | 只有托盘扫码入口，没有同名命令行参数；本地文件可用 `--barcode-decode` |
| `barcode-decode FILE` | 实际参数 `--barcode-decode FILE`，依赖 zbarimg |
| `empty-group` | 实际参数 `--empty-group`，有运行实例命令分发 |
| `show-group-manager` | 实际参数 `--show-group-manager`，有运行实例命令分发 |
| `whiteboard --color / --active-screen` | 实际为 `--whiteboard [color]`，支持 `--active-screen`；没有 `--color` 参数 |

## 验证范围

本轮检查设置窗口实际控件的保存、取消、重开、即时应用、错误输入、密钥遮蔽和设置文件保留；用本机模拟服务验证图片识别、文字翻译、图片翻译、auto 不调用本地识别、鉴权失败、错误响应、空响应和超时。模拟服务不代表真实供应商的兼容性、识别准确率或翻译质量。

Windows 依赖收集使用代表性依赖图检查递归收集、插件依赖、大小写差异、缺少下级库时失败，以及独立核对不借用开发环境库。当前主机为 macOS，Windows 干净路径启动检查已加入发布流程但尚未运行；不能报告用户那台 Windows 已修复。
