# 设置窗口重新设计（2026-10-17）

本次直接修改桌面应用的设置窗口，不是另做一个网页概念图。沿用原有六类设置和保存语义。

## 设计依据

参考 Apple 当前的官方指南：

- [Settings](https://developer.apple.com/design/human-interface-guidelines/settings)：让设置易于查找、按用途组织。
- [Sidebars](https://developer.apple.com/design/human-interface-guidelines/sidebars)：以稳定的侧栏作为分类导航。
- [Materials](https://developer.apple.com/design/human-interface-guidelines/materials)：区分导航与内容层次，避免影响正文和表单的清晰度。

本次采用侧栏、彩色分类图标、分组设置行、克制的分隔线、清晰的标题层级和蓝色主操作。浅色、深色分别设置文字、背景、输入框和分隔线颜色；“跟随系统”响应系统外观变化。没有宣称接入原生 Liquid Glass，也没有用透明效果遮挡表单。

默认窗口 920 × 700，最小窗口 780 × 580。较长页面在右侧独立滚动，左侧导航与底部保存操作保持可见。服务密钥默认隐藏，可临时显示。输入错误自动跳到对应分类并定位字段；修改输入后清除过期错误标记。保存后立即生效，取消不改动配置。

## 实际验证

- macOS 27.0.1 原生桌面运行：设置保存/重开、保存后即时应用、取消、侧栏点击及方向键、开关点击及空格键、密钥显示/隐藏、错误定位。
- 六个页面 × 浅色/深色 × 两种窗口尺寸，共生成并检查 24 张页面截图；检查没有水平溢出、底部操作保持在窗口内。
- 完整检查通过：核心检查、43 项界面与模拟服务检查、4 项 Windows 依赖收集检查。
- 模拟服务覆盖识别、文字翻译、图片翻译和失败处理，不代表真实供应商的识别质量或兼容性。当前主机不是 Windows；Windows 成品启动检查仍需发布流程在 Windows 环境执行。

代表性截图保存在 [验证目录](verification/settings-design-2026-10-17/)。
