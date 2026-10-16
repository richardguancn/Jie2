# 截2（jie2）

跨平台截图 / 标注 / 贴图工具。对标 Snipaste 2.x 专业版全功能，并在 Linux aarch64 上可用。

> 当前状态：**M0–M7 主要功能全部落地**（截图 / 标注 / 衣架 / 贴图 / OCR / 命令行 / 打包）。
> 功能规划与选型依据见 [《截2-可行性评估报告.md》](截2-可行性评估报告.md)。
> 剩余细化项（贴图组命令、Windows 平台支持、layer-shell、单实例 IPC）见文末。

## 架构：三层解耦

```
┌─ ui ──────────────  Qt 6（Widgets / QML 可替换）
│                    托盘、设置界面、衣架外观、贴图窗口
├─ platform ────────  平台集成层（每平台一个实现）
│                    抓屏、全局热键、置顶窗、layer-shell、剪贴板
└─ core ────────────  图像内核（纯 C++）
                     几何、像素缓冲、配置、能力模型；
                     后续：标注渲染、马赛克、模糊、取色、撤销栈、OCR 调度
```

**依赖方向严格向下**：`ui → platform → core`，禁止反向依赖。

这条约束不是洁癖，它对应三个具体的工程目标（评估报告 3.5.3）：

1. **UI 技术可替换** —— 将来若把 UI 层从 Widgets 迁到 QML，只重写 `src/ui`，内核与平台层不动。
2. **平台差异被收敛** —— X11 / Wayland / macOS 的差异只存在于 `src/platform` 的某个实现里，不渗入业务代码。
3. **内核可脱离 GUI 单测** —— `jie2_core` 不依赖 Qt 与任何系统 API，可在无桌面会话的 CI 容器里跑测试。

## 目录结构

```
CMakeLists.txt              顶层工程（Qt 可选：无 Qt 时只构建下两层）
src/core/                   图像内核（零 UI 依赖）
  geometry.h                Point / Size / Rect
  image_buffer.{h,cpp}      RGBA/BGRA 像素缓冲
  image_ops.{h,cpp}         裁剪/拼接/缩放/像素比对 + 边框/阴影/圆角 + 马赛克/模糊
  annotation.{h,cpp}        标注模型与撤销栈（M2）
  snap.{h,cpp}              窗口吸附几何（M1，纯函数可单测）
  capability.{h,cpp}        平台能力模型（会话类型 + 能力状态 + 摘要文本）
  capture_history.{h,cpp}   截图历史（回放 + 衣架数据源）
  config.{h,cpp}            键值配置（INI 风格，宽松解析）
src/platform/               平台集成层
  platform.h                Platform 接口（探测 / 热键 / 抓屏 / 窗口几何）
  display.h                 显示器与抓屏结果类型
  hotkey.h                  热键描述类型
  factory.cpp               平台分发 + 未支持平台兜底实现
  macos/macos_platform.mm   Carbon 热键 + CGDisplay 抓屏 + CGWindowList 窗口枚举
  linux/linux_platform.cpp  X11 XGrabKey + XGetImage 抓屏 + XQueryTree 窗口枚举
src/ui/                     UI 层（唯一允许使用 Qt 的地方）
  main.cpp                  GUI 入口（托盘常驻 + 自动化验证入口）
  tray.{h,cpp}              托盘控制器（截图/装饰/触发角/衣架/贴图/OCR）
  selection_overlay.{h,cpp} 截图选择遮罩（框选/吸附/放大镜/回放/12 键标注工具条）
  annotation_render.{h,cpp} 标注渲染（矢量 QPainter + 像素效果合成）
  clothesline.{h,cpp}       衣架（M3，tendedero 合并）
  pin_window.{h,cpp}        贴图窗口（M4：置顶/旋转/翻转/缩放/透明度/穿透/反色）
  ocr_runner.{h,cpp}        OCR 执行器（M5：tesseract/自定义命令/OpenAI 兼容端点）
src/tools/                  CLI 工具（不链接 Qt）
  probe_main.cpp            jie2-probe：平台能力/抓屏/窗口枚举，CI 与现场诊断
tests/core_tests.cpp        内核单元测试（几何/缓冲/配置/能力/吸附/标注）
scripts/
  verify-linux-aarch64.sh   Linux ARM64 构建+验证（容器）
  test-interactive-linux.sh 交互链路 E2E（选择→绘制→撤销→重做→保存）
  test-snap-linux.sh        窗口吸附 E2E
  test-clothesline-linux.sh 衣架 E2E（展开/复制/收起）
  color_window.c            确定色靶窗（抓屏像素验证用）
  window_target.c           普通顶层窗口（吸附验证用）
  mouse_tool.m              macOS 合成鼠标/键盘（交互验证用）
```

## 构建

### macOS

```bash
brew install qt cmake
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
```

### Linux（含 aarch64）

```bash
sudo apt install build-essential cmake qt6-base-dev libx11-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
```

Qt6 缺失时仍可构建 `core` / `platform` / `tools` / `tests`（UI 层自动跳过）。

## 验证

```bash
# 单元测试（任何环境）
cd build && ctest --output-on-failure

# 平台能力探测（无 GUI 依赖）
./build/src/tools/jie2-probe

# 全局热键注册实测（Linux 下建议在 Xvfb 或有 X 会话中运行）
./build/src/tools/jie2-probe --test-hotkey
```

`--test-hotkey` 会做两次注册：首次注册同一组合，若系统层面真的生效，
第二次会被拒绝（macOS 返回 `eventHotKeyExistsErr`），这是"注册成功"的独立佐证。

### Linux aarch64 容器验证

```bash
docker run --rm -v "$PWD":/work -w /work ubuntu:24.04 \
    bash scripts/verify-linux-aarch64.sh
```

## 平台能力模型

`PlatformCapabilities` 如实报告当前会话下每项能力的真实状态，
而不是假设"代码写了就一定可用"（评估报告 3.2 要求降级必须对用户可见）：

| 状态 | 含义 |
|---|---|
| `available` | 直接可用，无额外授权 |
| `degraded` | 可用但受限（需授权 / 每次弹窗 / 功能缩水） |
| `unavailable` | 当前会话不可用 |
| `n/a` | 该平台无此概念（如 macOS 没有 layer-shell） |

已知的平台差异（M0 实测）：

| | macOS | Linux X11 | Linux Wayland |
|---|---|---|---|
| 抓屏 | 需屏幕录制权限（真实探测） | XGetImage 直通 | **portal Screenshot（已实现，可能弹一次授权）** |
| 全局热键 | Carbon | XGrabKey | **portal GlobalShortcuts（已实现）** |
| 置顶窗 | NSWindow level | EWMH | 合成器相关 |
| 顶部条 | NSPanel | 置顶窗 | layer-shell（GNOME 不支持） |

## 验证状态（M0 + M1 实测）

| 项目 | macOS arm64 | Linux aarch64 |
|---|---|---|
| 全量构建（core/platform/ui/tools/tests） | ✅ | ✅ |
| 内核单元测试 | ✅ | ✅ |
| 平台探测 | ✅ 真实读取系统版本 / 屏幕数 / 屏幕录制权限 | ✅ X11 / Wayland / 无会话 三态均正确 |
| 全局热键注册 | ✅ Carbon（二次注册被系统以 `eventHotKeyExistsErr` 拒绝，作为独立佐证） | ✅ XGrabKey（Xvfb 虚拟 X11 下实测） |
| GUI 启动冒烟 | ✅ | ✅（Xvfb） |
| Wayland 降级如实报告 | — | ✅ 四条能力逐条标注受限原因 |
| **抓屏：全屏** | ✅ 3420x2214（2x Retina） | ✅ 640x480（Xvfb） |
| **抓屏：区域** | ✅ 100x100 逻辑点 → 200x200 物理像素 | ✅ 100x100（X11 无缩放，1:1） |
| **抓屏：像素级正确性** | ✅ 与系统 `screencapture` 逐点比对：57 点零差异 | ✅ 与 `xwd` 双工具对照：已知色窗口 #FF0000 / #00FF00 完全一致 |
| **截图链路：UI 层**（抓屏→QImage→PNG→剪贴板） | ✅ `--capture-once` 产出 3420x2214 PNG；与系统工具再比对：35 点平均差 0.17 | ✅ `--capture-once` 产出 640x480 PNG，中心像素 = 靶色 |
| **选择遮罩：像素精确裁剪** | ✅ 靶窗内 100×100 逻辑点 → 200×200 全同色（40000 像素零杂色）；边界恰在 x=19/20 之间 | ✅ 蓝靶块内 400 采样全 (0,0,255)；块外全黑 |
| **选择遮罩：交互流程** | ✅ 拖拽→尺寸标签→回车→保存文件恰为标签尺寸 | ✅ Xvfb 下遮罩启动稳定（无真实鼠标，交互部分由 macOS 侧覆盖） |
| **选择遮罩：工具条** | ✅ 点击「保存」按钮 → 1402×802 与标签一致 + 剪贴板同步 | —（按钮位置由 macOS 侧覆盖） |
| **选择遮罩：脚本入口** | ✅ `--select-rect x,y,w,h` | ✅ `--select-rect` 输出与靶色一致 |
| **历史回放 + 裁剪**（`,`/`.` → 拖拽 → 保存） | ✅ **逐像素零差异**（1681/1681 像素一致） | ✅ **逐像素零差异**（1681/1681，且跨色块边界验证对齐） |
| **F5 刷新截图** | ✅ 隐藏遮罩→重抓→恢复，日志确认 | ✅（同一代码路径） |
| **输出装饰**（圆角+边框+阴影） | ✅ 200×200 → 276×276；中线恰 20 个绿色边框像素（10px×2 边）；四角透明 | （同一内核函数，见下） |
| **色彩管理**（sRGB 嵌入） | ✅ PNG 含 `iCCP` 块（288B），`sips` 报告 `profile: sRGB` | —（Qt 同一写码路径） |
| **窗口吸附** | ✅ 候选 7 窗口；判定逻辑单测覆盖 7 组场景 | ✅ **E2E**：拖至 (996,436) → 精确吸附到窗口边缘 (1000,440) |
| **触发角** | ✅ 停留后日志"触发角触发截图"→ 遮罩打开（候选 7 窗口） | （同一代码路径） |
| **标注合成**（矩形/椭圆/箭头/马赛克/模糊） | ✅ 逐像素：边框色精确 (239,68,68)、椭圆顶点色精确、箭头 404 蓝像素、马赛克块内 100/100 同色、模糊区平滑 | ✅ 同断言通过（`--test-annotate`） |
| **标注交互 E2E**（选择→绘制→撤销→重做→换色→保存） | —（激活限制，交互由 Linux 侧覆盖） | ✅ 完整通过：选区 400×300、矩形 151/151 红像素、箭头 390 黄像素、输出尺寸精确 |
| **衣架 E2E**（展开/复制/收起） | ✅ 展开/收起逻辑（日志），抖动修复后单次展开 | ✅ **完整通过**：2 项初始化、顶边展开、点击复制（剪贴板 465B PNG）、离开收起、条带像素 4653/5372 命中 |
| **贴图变换 M4**（旋转/翻转/反色） | ✅ 逐像素：90°顺时针后四象限位移精确、水平翻转 TL↔TR 互换、反色红→青 | ✅ 同断言通过（`--test-pin`） |
| **标注增补 M2**（文字/序号） | ✅ 文字 404 红像素、序号圆 (239,68,68) + 白色数字 46 像素 | ✅ **E2E**：文字输入框收到键入 "TEST"（119 黄像素）、两次点击生成序号 1/2（572/553 黄像素） |
| **OCR M5** | ✅ 系统 Vision：`HELLO WORLD 123` 精确识别 | ✅ tesseract：同结果；自定义命令行后端（真实脚本）：同结果；auto 回退链正常 |
| **命令行 M6** | ✅ `--output` 存到指定路径（240×160 精确）；`--round 12 --border 6 --shadow` 组合生效（160×120→228×188，四角透明） | （同一代码路径，见 macOS 列） |
| **橡皮擦 / 圆角 / 虚线 M2** | ✅ 圆角矩形角点无线、边中点有像素；虚线有无交替 14 次（是真虚线） | ✅ **E2E**：画矩形 → 橡皮擦拖过后该区域红色像素 0 |
| **椭圆形状 / 双向箭头 / 放大镜 M2** | ✅ 逐像素：椭圆马赛克中心 (127,0,127)、椭圆四角保留原噪声；双向箭头两端各 113+ 黄像素；放大镜块左白右黑（跨分界的源区被真实放大 2x） | ✅ 同断言通过 + 核心椭圆掩码单测 |
| **衣架动画 M3** | ✅ 展开/收起逻辑 | ✅ **逐帧**：y 采样 -150→-136→…→0（11 个中间位置）；滑出对称 |
| **剪贴板监听 / 拖出 M3** | — | ✅ **E2E**：xclip 写入红图 → 历史 +1 且带 copied 前缀；按住卡片拖动触发 QDrag；无接收者时卡片保留 |
| **白板 / --size / --active-screen M6** | ✅ `--size`：120×80 → 200×100 | ✅ **E2E**：白板蓝底 + 红色标注（724 像素）→ 保存 400×320；`--size` 60×40→180×90 |
| **条码识别 M6** | （需装 zbar：`brew install zbar`） | ✅ **E2E**：qrencode 生成二维码 → 内容精确匹配；`--barcode-decode` 读出 URL；无码图不误报 |
| **贴图分组 M4** | ✅ 12 项断言全过（建组/切组/可见性/循环） | ✅ 同断言通过 |
| **调色板配置 M2** | （同代码路径） | ✅ **E2E**：palette_size=3 → 黄/绿/红/黄 循环，第 4 次回到第 1 色 |
| **Wayland GlobalShortcuts M7** | — | ✅ **E2E（mock 门户）**：能力探测识别服务（version=2）、绑定成功、Activated 触发回调、触发串 "CTRL+ALT+t" 正确、拒绝场景如实失败；**25 轮连续通过** |
| **Wayland Screenshot M7** | — | ✅ **E2E（mock 门户 + 像素核对）**：全屏抓取（源图 5 种行滤波轮转、4 点像素核实）、区域裁剪（2 点核实）、越界/取消如实失败；**5 轮连续通过** |
| **打包 M7** | ✅ **自包含包**：`--bundle-qt` 产出 12MB zip（5 个 Qt 框架 + 9 个插件 + 重签名）；dyld 验证全部从包内加载；GUI 运行正常 | ✅ **双格式**：deb（2.4MB，安装后运行正常）+ AppImage 风格自解压包（3.7MB，GUI 冒烟通过、`--version` 正常） |

抓屏验证细节：

- **macOS 交叉验证**：用系统自带 `screencapture` 抓同一屏，转 BMP 后与我们的
  PPM 逐点比对（57 个采样点，含四角/中心/随机点）——最大通道差 **0**。
- **Linux 已知色验证**：`scripts/color_window.c` 创建一个确定颜色的
  override-redirect 窗口作为靶子（在 Xvfb 下 `xsetroot` 对 root window
  的着色读不到，必须用真实窗口），jie2 与 `xwd` 双工具结果一致。
- **macOS 两个反直觉的实测结论**（已写入代码注释，避免后人踩坑）：
  1. `CGDisplayPixelsWide` 在 macOS 27 返回逻辑点而非物理像素，
     缩放比例须改用 `CGDisplayMode` 计算。
  2. `CGDisplayCreateImageForRect` 的 rect 参数用**逻辑点**，
     返回图像却是**物理像素**（1710x1107 输入 → 3420x2214 输出）。

> 排障开关：Linux 抓屏加 `JIE2_X11_DEBUG=1` 可打印 XImage 的
> 位深 / 字节序 / 掩码 / 首字节，便于在不同 X 服务器上核对布局。

实测环境：

- macOS：macOS 27.0.1 / arm64 / Qt 6.11.2（`qtbase`，见下方说明）
- Linux：Debian 12 (bookworm) / **aarch64** / Qt 6.4.2 / libx11-dev / Xvfb

> macOS 上 Qt 6 通过 `brew install qtbase` 安装（只需 Core/Gui/Widgets）。
> 安装时为解决与 `qt@5` 的链接冲突执行过 `brew unlink qt@5` ——
> 该操作只移除符号链接，Qt 5 文件仍保留在 Cellar，需要时用
> `brew unlink qtbase && brew link qt@5` 切回。

## 里程碑

见评估报告第五节。已完成：

- **M0 骨架** —— 三层结构、平台探测、全局热键（两平台实测通过）、托盘、配置持久化。
- **M1a–M1d 抓屏地基** —— 图像内核（裁剪/拼接/缩放/像素比对）、平台层抓屏接口、
  macOS 与 Linux X11 双实现、双平台像素级验证（见上表）。
  命令行可试：`jie2-probe --displays`、`--capture`、`--capture-region x,y,w,h`、
  `--capture-save out.ppm`、`--windows`（可吸附窗口列表）。
- **M1e-1 截图闭环** —— 托盘菜单「截图」、全局热键 `Ctrl+Alt+T` 触发截图、
  保存至图片目录 `jie2/` 子目录并写入剪贴板；`jie2 --capture-once` 为自动化验证入口。
- **M1e-2 截图选择 UI** —— 全屏冻结遮罩：拖拽框选、选区外变暗、实时尺寸/起点标签、
  **可视化工具条（保存 / 取消按钮，点击优先于拖拽，避免误清选区）**、
  4x 放大镜 + 光标色值（按 `C` 复制）、回车/双击确认、Esc/右键取消；
  保存与剪贴板复用同一条落点链路。脚本入口 `--select-rect x,y,w,h`
  （供自动化与 M6 命令行复用；`--select` 打开交互遮罩）。

- **M1e-3 截图历史回放** —— `,`/`.` 在遮罩内回放最近 16 张截图（历史持久化在配置目录
  `history.txt`）；选历史图后保存，内容从历史文件裁剪（非当前屏）。双平台逐像素验证。
- **M1e-4 F5 刷新截图** —— 遮罩内按 F5 重新抓屏；实现上先隐藏遮罩、等合成器完成
  一次合成（150ms）再抓，避免把遮罩自己拍进去。
- **图像内核装饰函数**（M1 输出项的基础件）—— `addBorder` / `addShadow` /
  `roundCorners` / `pixelate`（马赛克）/ `boxBlur`（高斯模糊近似，三次 box blur），
  全部纯 C++、带单测。UI 接线（截图时应用边框/阴影/圆角）为下一项。

- **M1 收尾四项（全部完成）**——
  - **输出装饰 UI 接线**：托盘菜单「输出装饰」开关圆角（半径可配）/边框（宽度、颜色）/
    阴影；按「圆角 → 边框 → 阴影」顺序在保存链路上应用。
  - **窗口吸附**：平台层新增 `windowRects()`（macOS `CGWindowListCopyWindowInfo`、
    X11 `XQueryTree`），拖拽时四边各自吸附到窗口边缘（8pt 阈值），
    吸附边有高亮指示；吸附几何抽为 `core::snap` 纯函数并单测。
  - **触发角**：屏幕角落停留（可配时长/尺寸/角落组合）触发截图，
    触发后需离开角落才重新武装；遮罩打开期间自动抑制。
  - **色彩管理**：保存时嵌入 sRGB（PNG `iCCP` 块；`sips` 报告 `profile: sRGB`）。
- **M2 标注（核心工具完成）**—— 选择遮罩内置 12 键工具条：
  矩形 / 椭圆 / 箭头 / 铅笔 / 马克笔 / 马赛克 / 模糊 + 撤销 / 重做 / 颜色 / 保存 / 取消；
  程序化验证入口 `jie2 --test-annotate <out.png>`。
  撤销重做（Ctrl+Z / Ctrl+Shift+Z）、滚轮调粗细、数字键 1-7 切工具。
- **M3 衣架（tendedero 合并，完整）**—— 顶部条带窗口：指针停到屏幕顶边**滑入动画**展开，
  展示最近截图缩略图；单击复制回剪贴板、右键从衣架取下（文件保留）、
  指针离开或 Esc **滑出动画**收起；截图前自动收起（避免被拍进截图）。
  - **滑入滑出动画**：定时器驱动的窗口位移（12ms 步进、14px/步）。逐帧采样验证：
    收起态 y=-150 → -136 → … → -10 → 0，确证是逐步动画而非跳变。
  - **拖出保存**：按住卡片拖到文件管理器/聊天窗口 —— 文件 URL + 图像双载荷；
    对方以 MoveAction 接收时卡片下线（对齐 tendedero 的 "Drag into a folder"）。
  - **剪贴板图像监听**：复制图像（截图工具、浏览器等）自动挂上衣架，
    对齐 tendedero 的 "Hang copied images"。带文本/URL 的复合内容跳过（避免误挂）；
    内容级去重（32×32 摘要），自己写入剪贴板的内容不会被再次挂载。
- **M2 标注增补（文字 / 序号 / 橡皮擦 / 圆角 / 虚线完成）**—— 工具条扩到 15 键（10 工具 + 撤销/重做/颜色/保存/取消）：
  - **橡皮擦**：按住拖过标注即整条删除（半透明提示语义：矢量标注被擦一半没有合理语义，
    按"包围盒相交即整条删"工作）；擦除会清空 redo 栈（与"新添加即清空"一致）。
  - **虚线样式**：`D` 键切换，作用于矩形/椭圆/箭头/铅笔/折线（专业版对齐项 v2.5）。
  - **圆角矩形标注**：`R` 键循环半径 0/8/16/24（专业版对齐项 v2.4）。
  - **文字**：选区内点击弹出内联输入框（回车提交 / Esc 取消），带描边保证任意底色可读；
    按 `T` 快速切换；字号随"粗细"维度调节（滚轮/数字键）。
  - **序号**：点击放置自动编号的圆形标记（1、2、3…）；编号基于当前可见数量，
    撤销后编号自动重用；按 `N` 快速切换。
  - **椭圆形状**（`O` 键，专业版 v2.6）：马克笔/马赛克/模糊/橡皮擦以椭圆工作；
    椭圆掩码语义在核心里单测（中心被处理、四角保留原像素）。
  - **箭头样式**（专业版 v2.5）：`B` 双向箭头、`Shift+B` 反向箭头。
  - **放大镜标注**（`M` 键，专业版 v2.6）：拖出源区域，合成时将该区域内容
    放大 N 倍呈现（源框与放大块都有描边，位置自动收进画布）。
- **M4 贴图（核心完成）**—— `Ctrl+Alt+P` 或托盘菜单把剪贴板内容贴成置顶窗口：
  - 来源（对齐 Snipaste 五类）：图像 / 纯文本（渲染为图片）/ 颜色值（色块 + HEX 标注）/
    图像文件路径（加载图像）/ 非图像文件路径（当文本贴出）
  - 操作：滚轮或 `+`/`-` 缩放、`1`/`2` 旋转、`3`/`4` 翻转、`Ctrl+滚轮`透明度、
    `6` 反色、中键重置 100%、`Shift+双击` 缩略图模式、拖动移动、`Esc`/双击关闭
  - 鼠标穿透（`Qt::WindowTransparentForInput`）、隐藏/显示全部、关闭全部、
    保存全部（批量保存到截图目录，`saveTo` 单张/批量共用同一路径）
  - 变换是像素精确的：对 90° 旋转用整数变换（非插值），逐像素验证通过
- **M5 OCR 子系统（核心完成）**—— 可插拔后端（评估报告 4.4）：
  - **系统后端**：macOS Vision（离线、零配置，实测 "HELLO WORLD 123" 精确识别）
  - **tesseract**：跨平台离线（aarch64 由发行版提供），CLI 后端
  - **自定义命令行**：stdin 收 PNG / stdout 出文本的契约，示例脚本 `scripts/ocr-custom-example.sh`
  - **OpenAI 兼容端点**：自定义 base_url + model + key（云端，使用前弹窗确认"会上传图像"）
  - auto 模式回退链：系统 → tesseract，两条失败时把两条原因都报出来
  - 入口：托盘菜单「文字识别（OCR）」，结果写回剪贴板
- **M6 命令行（专业版选项对齐）**—— 脚本化输出、装饰覆盖与自动化入口：
  - `--output <path>` / `-o <path>`：显式输出路径（按扩展名存 PNG/JPEG）
  - `--no-save`：只写剪贴板不落盘
  - `--shadow` / `--no-shadow` / `--border N` / `--round N`：本次调用的装饰覆盖
    （不写入配置文件 —— 命令行调用不改变用户的持久设置）
  - `--size W H`：强制输出尺寸（实测：60×40 源 → 180×90 输出）
  - `--active-screen`：用光标所在屏而非主屏
  - `--hold`：截图后先标注再输出（等价于打开交互遮罩，对齐 Snipaste 的 `snip --hold`）
  - `--block [SECONDS]`：兼容参数（本 CLI 天然同步，命令返回即完成）
  - `--whiteboard [color]`：**白板模式** —— 纯色画布 + 全套标注工具，保存标注结果
    （实测：蓝底画布 + 红色矩形，输出 400×320 且底色/标注逐像素正确）
- **M2 增补：调色板数量可配置**（专业版对齐项）—— 10 色内置调色板，
  配置 `annotation.palette_size` 控制循环数量（2~10）。
  实测：设为 3 时颜色按 黄→绿→红→黄 循环，第 4 次回到第 1 色。
- **M4 增补：贴图分组管理**（专业版对齐项）—— 默认 3 组，托盘菜单「贴图分组」切换；
  只显示当前组的贴图、其余隐藏（保留在内存与列表）；"隐藏全部"与分组正交。
  实测 12 项断言：建 2 张 → 组 0 可见 2；切组 1 → 可见 0；组 1 建 1 张 → 可见 1；
  切回组 0 → 可见 2；"下一组"循环正确。
- **M6 增补：二维码/条形码识别**（专业版对齐项 `barcode-scan` / `barcode-decode`）——
  zbar 后端（`zbarimg`）+ 托盘菜单「识别二维码/条形码」+ CLI 文件入口。
  实测：qrencode 生成 "JIE2-BARCODE-TEST" → 识别内容精确匹配；
  `--barcode-decode` 正确读出 URL；无码图返回 "no barcode found"（不误报）；
  未装 zbar 时给出三个平台的安装命令（不静默失败）。
- **M7 前置：Wayland 全局热键（portal GlobalShortcuts，完成）**——
  Wayland 没有全局热键核心协议，唯一合规路径是 xdg-desktop-portal 的门户
  （GNOME 48+ / KDE Plasma 6.1+ 已支持）。`src/platform/linux/wayland_portal.cpp`
  用 libdbus-1 直连实现（platform 层不依赖 Qt 的约束保持不变）：
  - 完整流程：CreateSession → BindShortcuts → 监听 Activated → Session.Close
  - **每快捷键一个会话**（官方文档明确"每会话只能绑定一次"）
  - 触发串按规范生成（"CTRL+ALT+t"）；能力探测真实查询门户服务与接口版本
  - 构建开关：`libdbus-1` 存在则启用（Debian 系 `libdbus-1-dev`），
    缺失时 Wayland 热键如实报告不可用，X11 后端不受影响
  - 验证：`scripts/test-wayland-portal.sh` 用 `scripts/mock-portal.py`
    （按规范实现的门户 mock）做端到端测试——绑定成功 / Activated 触发回调 /
    拒绝场景如实失败，**25 轮连续稳定通过**
- **打包三格式（deb / rpm / 自解压 AppImage）**——
  `scripts/build-deb.sh`、`scripts/build-rpm.sh`、`scripts/build-appimage.sh`。
  AppImage 是**简化自解压格式**（POSIX shell 头 + tar.gz 尾）：零外部工具、
  零 FUSE 依赖（标准 type-2 AppImage 需要 libfuse2，老国产系统常缺）；
  代价是不能挂载，换来"拷过去 chmod +x 就能跑"。脚本含自检
  （标记行 / 可执行 / 体积）并验证过实际运行（`--version` 输出正确）。
  2026-10-16 该脚本曾被意外截断为 0 字节（详见"已修缺陷"末尾），
  此后建立了 git 仓库做版本保护（提交 2025ba4 起）。
- **M7 前置：Wayland 截图（portal Screenshot，完成）**——
  `src/platform/linux/wayland_screenshot.cpp`：一次性请求-响应流程
  （调用 → 等 Response → 读回门 Pro 产出的 PNG 文件 → 解码）。
  配套 `src/platform/linux/png_decode.{h,cpp}`（基于 zlib 的最小 PNG 解码器，
  支持 8-bit 灰度/RGB/灰度+Alpha/RGBA、非隔行；其余如实报错）。
  **为什么解码器在平台层**：core 的硬约束是"只依赖 C++ 标准库"，
  而解码需要 zlib —— 这属于"平台形态 → core 形态"的转换器。
  - 集成语义：`captureAll()` 走门户全屏图；`captureRegion()` 在全屏图上
    按 (0,0) 原点裁剪（单显示器与"先整屏截取再选区域"的流程下成立），
    越界如实报错；`captureDisplay()` 在 Wayland 无法枚举显示器 →
    如实报错而非猜测目标屏
  - 构建开关：`libdbus-1` + `zlib` 都在才启用（`JIE2_HAVE_PORTAL_SCREENSHOT`）
  - 验证：`scripts/test-wayland-capture.sh`（12 项断言：全屏像素核对
    （源图含 5 种行滤波轮转）/ 区域裁剪 / 越界失败 / 用户取消失败），
    **5 轮连续稳定通过**
- **M7 打包（三平台完成）**——
  - **Linux deb**（`scripts/build-deb.sh`）：`dpkg -i` 安装后 `/usr/bin/jie2`
    与 `jie2-probe` 可用（容器内全链路验证）
  - **Linux AppImage 风格单文件**（`scripts/build-appimage.sh`）：免 root、免安装，
    AppRun 预检宿主 Qt 库缺失时给出各发行版安装命令
  - **macOS zip**（`scripts/build-macos.sh`）：默认轻量包；`--bundle-qt` 产出
    **自包含包**（12MB）—— 复制 Qt 框架依赖闭包 + 平台插件 + qt.conf + ad-hoc 重签名。
    已验证：dyld 全部从包内加载 Qt、GUI 正常运行、双热键注册成功
  - 三者共用 `packaging/make-icon.sh` 生成图标（与程序内托盘图标同源几何）
  - Linux 依赖策略：**用发行版自带 Qt**，不捆绑运行时 —— 评估报告 3.1 对
    麒麟/统信等老系统的兼容性要求

剩余细化项（不影响主链路，按需推进）：

- ~~**M4 增补**：贴图分组管理~~ ✅ 已完成（建组/切组/可见性/循环，12 项断言）。
- **M6 增补**：条码/二维码识别 ✅ 已完成；贴图组命令（`--empty-group` 等）
  待做 —— 它需要**先有单实例 IPC**：命令要作用于运行中的实例，
  而当前架构下 CLI 调用是独立进程（见下节大项 A）。
- **M7 增补**：Wayland portal —— ✅ **GlobalShortcuts（热键）与
  Screenshot（截图）双门户已完成**；ScreenCast（流式抓屏）经评估不做：
  一次性截图场景由 Screenshot 门户覆盖，ScreenCast 是持久流方案，
  留作将来录屏类需求的扩展点。rpm 包 ✅；macOS 安装包 ✅（自包含 .zip）。
- **M2 增补**：标注二次编辑（撤销到目标图案后重做，手册中的既有交互；
  Snipaste 手册亦标注为"将来支持"，现有替代 = 撤销/重做栈）。
- **Wayland 衣架（layer-shell 顶部条）**：待实现。收益范围仅 KDE/wlroots
  （GNOME 对 layer-shell 与置顶窗都不支持，无解）。需要自绑
  wlr-layer-shell 协议（Qt6 下无现成库，Debian 的 LayerShellQt 是 Qt5），
  可在容器内用 Sway headless 做真实合成器验证。

### 尚未开始的大项

- **A. 单实例 IPC**：让 CLI 命令作用于运行中的实例（`--empty-group`、
  `--show-group-manager` 与"二次启动唤起主实例"都依赖它）。
  需要 QLocalServer 命令通道与 main 的启动流程改造。
- **B. layer-shell 顶部条**（见上）。
- **C. Windows 平台支持**（现状如实说明）：
  - 代码现状：`src/platform/` 下**只有 `linux/` 与 `macos/` 两个实现**，
    Windows 走 `factory.cpp` 的"能力全 Unknown"空实现 —— 即**不能运行**。
  - 需要的工作：① 新增 `platform/windows/` 实现（Win32：BitBlt/GDI 抓屏、
    `RegisterHotKey` 全局热键、`SetWindowPos(HWND_TOPMOST)` 置顶窗、
    剪贴板 DIB 读写）；② Qt6 for Windows 工具链；③ Windows 构建与运行环境。
  - **为什么没有直接写代码**：当前开发环境（macOS 宿主 + aarch64 Linux
    容器）没有 Windows 运行/构建链（Qt6 Windows 交叉链 + 运行验证）。
    在无验证手段的情况下写大段平台代码违背本项目"能验证才声称完成"
    的原则 —— 该平台应作为独立专项，在具备 Windows 构建环境时开展。

### 已修掉的真实缺陷（累计）

1. **拖拽选区登记不上**（`selection_overlay.cpp`）：`mouseReleaseEvent` 先把
   `dragging_` 置 false 再调 `currentSelection()`，而后者在「非拖拽且无选区」时
   返回空 —— 结果拖拽后永远没有选区。正确顺序：先用拖拽状态算出选区，再翻转状态。
2. **Retina 缩放比例算错**（`tray.cpp`）：不能用「显示器 bounds ÷ 屏幕几何」求缩放
   （macOS 上两者都可能以逻辑点上报，得出错误的 1.0）。必须用
   **「实际抓到的图像宽度 ÷ 逻辑宽度」** —— 它描述的是真实拿到的像素。
3. **CLI 路径覆盖历史**（`tray.cpp`）：`--capture-once` / `--select-rect` 不走
   `start()`，配置与历史未加载，保存时用空状态**覆盖磁盘**（实测连截两张只剩一张、
   计数清零）。修复：加载移入构造函数。
4. **R/B 通道对调**（`tray.cpp`）：`toQImage` 写死按 BGRA（`Format_ARGB32`）解释
   缓冲，但 `fromQImage` 产出的是 RGBA —— 经它往返的图像（历史裁剪）红蓝互换
   （实测最大差 4 且呈对调模式）。修复：按缓冲声明的 `format()` 选择
   `Format_RGBA8888` 或 `Format_ARGB32`。
5. **选区多出 1 像素**（`selection_overlay.cpp`）：`QRect(topLeft, bottomRight)`
   是闭区间语义，拖 400 像素得到 401 宽。修复：`rectBetween()` 改为半开区间
   （`[left, right)`），"拖多远裁多大"。
6. **衣架在 macOS 上抖动**（`clothesline.cpp`）：窗口被窗口服务器下移到菜单栏
   之下，而光标仍在屏幕顶边（y=1~3）落在窗口上方 → "离开即收起、下个 tick 又
   展开"的循环。修复：保持展开的判定上边界取**屏幕顶边**而非窗口自身。
7. **文字提交后整个截图被误确认**（`selection_overlay.cpp`）：内联文字输入里按
   回车提交后，同一个回车事件继续冒泡到对话框的 `keyPressEvent`，命中"回车 =
   确认截图"分支 —— 表现为"文字之后的点击全部失效"（截图已保存并关闭）。
   修复：在事件过滤器里吞掉回车（`commitTextInput()` 后 `return true`）。
   实测教训：这类"跨控件按键语义冲突"必须考虑事件冒泡，不能只看单个控件的行为。
8. **OCR 测试图粗体导致误读**（`main.cpp` 的 `--test-ocr`）：tesseract 对粗体
   大写 H 常误读为 I、尾字符偶发丢失（同文案 bold/regular 对照实验证实）。
   修复：测试图改用常规字重。教训：合成测试图时"看起来像 OCR 出错"的问题，
   先排查测试图本身。
9. **衣架点击卡片段错误**（`clothesline.cpp` + `tray.cpp`）：衣架自己写剪贴板会
   重入剪贴板监听（监听回调重建 `entries_`，调用栈里的引用失效）。
   修复分三层：① 复制动作交给托盘控制器（衣架只发 `copyRequested` 信号）；
   ② 写入统一走 `writeClipboardImage()`，写入后记录摘要做**内容级去重**
   （X11 的剪贴板事件是异步的，同步标志拦不住）；③ 监听回调加防重入与
   启动预热（记录启动时剪贴板摘要，避免把遗留内容当"用户刚复制"）。
10. **头文件缺 include 引发级联错误 + 增量构建假象**（`clothesline.h` 等）：
    signals 里用了 `QString` 却没 include，gcc 下把 `QObject` 解析成类内嵌套名，
    刷出 759 个错误。而**增量构建在时间戳不巧时不会重编该文件**，
    于是"构建看起来成功、测试跑的是旧二进制"，让排查绕了大弯。
    修复：补 include；新增 `scripts/check-headers.sh`（每个头单独 `-fsyntax-only`
    验证自包含）纳入回归；干净全量构建验证。
11. **Docker Desktop bind mount 缓存**（环境问题，非代码）：宿主上用原子替换
    写文件（写临时文件再 rename）后，容器内可能仍看到旧 inode 的内容 ——
    实测 `md5` 不一致但挂载配置正确。这会让"改了没生效"反复出现。
    修复：新增 `scripts/sync-to-container.sh`，构建前 `docker cp` 强制同步
    并校验关键文件摘要一致。
12. **macOS 自包含打包的三个连环坑**（`build-macos.sh`）：
    ① 只复制主二进制**直接引用**的框架不够 —— QtGui 传递依赖 QtDBus，
       而本机有全量 Homebrew Qt 所以本机跑得好好的，到别的机器才炸；
    ② 引用有**两种形态**（主二进制用绝对路径、框架之间用 `@rpath`），
       过滤器只认一种就会静默漏项；
    ③ `install_name_tool` 改写后**签名失效**，macOS 直接 SIGKILL ——
       症状是 "Killed: 9"（不是 dyld 错，极易误判成依赖问题），
       必须由内向外重新 ad-hoc 签名。
     另有一处路径少一级：插件引用框架写成了 `@executable_path/../../Frameworks`
    （多一级），症状是"插件找到了但加载失败"；`@executable_path` 在加载插件时
    仍指主二进制目录，正确写法是 `../Frameworks`。
13. **测试/CLI 路径空指针**（`tray.cpp` 的 `rebuildMenu()`）：测试入口没有托盘菜单
    （`start()` 未调用），而分组切换等动作会调 `rebuildMenu()` → `menu_->clear()`
    对空指针解引用（`--test-pin-groups` 实测 SIGSEGV）。修复：入口处判空返回。
    教训：给"只在 GUI 模式存在的成员"加动作时，要么守卫，要么让测试路径也初始化它们。
14. **D-Bus 多线程竞争**（`wayland_portal.cpp`，Wayland portal 实现期最难的 bug）：
    libdbus 的同一连接不允许被多个线程同时操作。早期版本让调用线程执行
    `send_with_reply_and_block`（内部 poll+dispatch），worker 线程同时
    `read_write_dispatch` —— 实测主线程的 pending-call 完成通知会丢失：
    bind 的方法回复 +218ms 就到达（filter 可见），主线程却阻塞到 +1684ms
    才被"推醒"，而 mock 的 1500ms Activated 触发器恰好落在这个窗口里，
    信号到达时回调表还没登记 → 激活被静默丢弃（约 1/8 概率的偶发失败）。
    修复：改为**单线程 dispatch + 任务队列**（调用线程只入队+等结果，
    worker 是唯一碰连接的人）。
15. **单线程改造引入的死锁**（上条修复的"第二只靴子"）：worker 成为唯一
    dispatch 线程后，如果它只是阻塞在条件变量上等 Response，响应永远无人
    处理 —— 实测表现为每次 12 秒超时。修复：worker 的等待必须循环
    "检查 + 继续 dispatch"。教训：单线程 dispatch 模型里，"等待"和
    "推进消息"必须是同一个循环里的动作，不能分家。
16. **头文件检查项拖垮回归**（`check-headers.sh`）：6 个 Qt 头串行
    `-fsyntax-only` 要 100+ 秒，回归里 90 秒超时把它变成假失败。
    修复：改并行执行（-P 4），回归项超时给足 300 秒。
17. **`build-appimage.sh` 被意外截断为 0 字节**：某次会话操作中脚本损坏
    （具体原因未确定），而此前项目**没有版本控制**，恢复只能靠逆向旧产物
    （自解压格式 = shell 头 + tar.gz 尾，从包头读出确切结构，结合现存的
    AppDir 素材重建脚本）。恢复后加了自检（标记行 / 可执行 / 体积）与
    实际运行验证。**教训：该目录已有长期价值，必须进 git** ——
    随后完成仓库初始化（提交 2025ba4），此后任何文件损坏可 diff / 回滚。
18. **mock 门户的两个 dbus-python 坑**（`mock-portal.py`，供后人省时间）：
    ① `dbus.service.BusName(...)` 的返回值若不接住会立即被 GC →
    名字释放 → 客户端报 "The name ... was not provided by any .service
    files"（mock 看着"启动成功"却没人持有名字）；
    ② 取调用方唯一名要用方法装饰器的 `sender_keyword` 参数 ——
    `dbus.get_current_message()` 这个 API 在 dbus-python 里不存在
    （AttributeError）。

### 已知待办：截图色彩管理（如实记录）

实测发现：jie2 产出的 PNG 与系统 `screencapture` 的**数值**不同（最大通道差 26），
但把系统图按其嵌入的 Display P3 profile 转成 sRGB 后，两者**最大差降到 1**。
即：jie2 的像素语义已是 sRGB（数值正确、能还原屏幕观感），仅差**未嵌入 sRGB
profile 标记**——严格遵循色彩管理的程序会按"sRGB"解释无标记文件，影响有限；
但为对齐专业工具行为，应保存时嵌入显示器的 ICC profile（或提供色彩空间选项，
Snipaste 的更新日志里也有"[Mac] 修复保存 PNG 色差""取色器色彩空间选项"同类条目）。
记为 M1 收尾项。

## 许可

（待定）
