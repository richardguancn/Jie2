# 银河麒麟桌面 V10 ARM64 适配

本次目标是银河麒麟**桌面版 V10 SP1、ARM64**。构建使用 Qt5 和系统提供的运行库，
不要求在麒麟上安装 Qt6，不替换系统库，不捆绑其他发行版的 Qt。

麒麟 V10 存在不同硬件和更新版本，不能只凭“V10”断言所有设备兼容。
官方[产品白皮书](https://www.kylinos.cn/upload/1/kycms/20250617/1934877937389113344.pdf)
列出了 V10 SP1 的组件和硬件差异。本项目选用 Ubuntu 20.04 ARM64 的 GCC 9、
glibc 2.31、Qt 5.12.8 作为较旧的兼容性构建环境；它不是麒麟系统镜像，也不代替 UKUI 实机验证。
中标麒麟、麒麟服务器版、x86_64 和龙芯不在本次 ARM64 桌面安装包的适用范围内。

## 生成安装包

在已经启动 Docker 的开发电脑上，从项目目录执行：

```bash
bash scripts/build-kylin-arm64.sh --docker
```

脚本会创建 ARM64 构建环境，编译主程序、诊断工具和 Wayland 辅助程序，执行已有测试，
检查实际链接的运行库，再生成 `dist/kylin-v10-arm64/jie2_0.0.2_arm64.deb`。
随后会在不含开发工具的 Ubuntu 20.04 ARM64 容器里安装成品，检查启动、截图、
重复安装和卸载。任何必需检查失败，脚本都会失败退出。

首次构建需要联网下载构建环境和软件包。可以用 `JIE2_BUILD_JOBS=2` 降低编译并行数。
版本号取自项目根目录 `CMakeLists.txt`，上面的文件名对应当前 0.0.2。

在麒麟 ARM64 设备上直接构建，先通过设备已配置的软件源安装构建和验证依赖：

```bash
sudo apt install build-essential cmake pkg-config python3 python3-pil \
    qtbase5-dev libqt5svg5-dev qt5-image-formats-plugins \
    libx11-dev libxrandr-dev libdbus-1-dev zlib1g-dev \
    libwayland-dev wayland-protocols xvfb xauth \
    desktop-file-utils librsvg2-bin ca-certificates libssl1.1
bash scripts/build-kylin-arm64.sh --native
```

直接构建也会执行测试和检查，但不会自动安装或卸载宿主机的软件。
`scripts/test-kylin-package.sh` 会修改系统软件包，仅允许在一次性 Docker 容器中运行。

## 在麒麟设备上安装和检查

先确认系统和架构：

```bash
cat /etc/os-release
uname -m
dpkg --print-architecture
```

ARM64 设备通常分别输出 `aarch64` 和 `arm64`。把对应安装包复制到设备，进入存放目录后执行：

```bash
sudo apt install ./jie2_0.0.2_arm64.deb
jie2 --version
jie2 --self-check
jie2-probe
```

启动检查须在已登录的图形桌面终端中执行。`self-check passed` 表示界面初始化、PNG 读写、
HTTPS 支持检查通过；它不代表已连通某个在线识别服务。
如安装时提示依赖版本不满足，保留报错并在该设备的软件环境中重新构建，
不要手动替换系统的 glibc、Qt 或 OpenSSL。

安装后应在开始菜单中看到 J2，也可运行 `jie2` 启动。实机交付前需要实际确认：

1. 托盘能显示，菜单能打开，退出后可重新启动。
2. 默认截图快捷键可用；若与系统截图冲突，在设置中改用其他组合后再试。
3. 框选截图、标注、撤销重做、保存和复制到剪贴板正常。
4. 屏幕贴图能移动、缩放、关闭；图片暂存能展开并打开图片。
5. 在设备实际使用的缩放比例、多显示器配置下，选区与输出位置一致。
6. 卸载后开始菜单入口消失，用户截图和配置保留。

可选离线识别和扫码能力使用发行版工具：

```bash
sudo apt install tesseract-ocr tesseract-ocr-chi-sim zbar-tools
```

Wayland 会话还依赖桌面提供截图门户和全局快捷键门户；编译了相关支持不等于桌面一定提供服务。
`jie2-probe` 用于查看当前会话的能力，缺少服务时功能按程序显示的结果判断。
本次验证以 X11 为主，UKUI 托盘、物理热键、多显示器和 Wayland 门户须在目标设备验收。

## 本次修复范围

- 将图像内核实际使用的语言版本统一为 C++17，移除旧编译器不支持的默认比较写法。
- 显式链接旧系统需要的线程库，修复 Qt 5.12 的坐标、颜色、字符串和多边形接口差异。
- 支持明确选择 Qt5；指定界面库缺失时构建失败，防止只生成诊断工具却当作完整应用。
- 安装包从成品计算运行依赖，检查架构，增加多尺寸图标和安装、卸载后的菜单缓存更新。
- 增加 ARM64 旧环境构建、运行与安装检查，并接入发布流程。

构建与测试的实际结果见[本次验证记录](verification/kylin-v10-arm64-2026-10-25/README.md)；
没有麒麟设备实测记录时，不应表述为已通过麒麟实机验收。
