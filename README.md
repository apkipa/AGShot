# AGShot

一个 Windows 截图工具，使用 C++/Win32 与 Direct2D 绘制截图浮层，并用 Intel NPU 完成离线文字识别。

## 特性

- **区域截图**：全局快捷键唤起浮层（默认 F1，可在配置中修改），拖动选择区域，支持四边与四角微调。
- **智能选区**：基于 UI Automation 识别指针下的窗口与控件，推荐对齐的选区，也可随时改为手动拖动。
- **悬浮工具栏**：选中区域后出现，提供编辑选区、识别文本、复制到剪贴板、取消四个操作。
- **使用 Intel NPU 的 OCR**：文字识别由 PaddleOCR 的检测（DBNet）与识别（CTC）两个模型完成，推理通过 Level Zero 全部提交给 Intel NPU，CPU 只负责图像缩放与结果整理。模型不嵌入二进制，与程序一同分发，识别全程离线。
- **识别与界面解耦**：识别在系统线程池中执行，不阻塞浮层绘制；切换界面、关闭浮层或再次截图都不会中断正在进行的识别，结果只在原浮层仍存在时回填。
- **文本选取**：识别结果支持鼠标拖动选择、Ctrl+A 全选、Ctrl+C 复制并关闭、方向键移动光标、滚轮与 PageUp/PageDown 滚动。
- **常驻托盘**：程序没有主窗口，常驻通知区域；托盘菜单可打开设置、重载配置或退出，重复启动会唤醒已有实例。
- **配置热重载**：配置文件修改后自动生效，无需重启。

## 系统要求

- Windows 10 或更高版本，x64。
- OCR 需要 Intel NPU（例如 Core Ultra 系列处理器）以及随 NPU 驱动安装的 Level Zero 运行时。没有 NPU 时截图与标注仍可使用，识别会返回错误信息。
- 构建需要 Visual Studio 2022（v143 工具集）、vcpkg（依赖见 `vcpkg.json`）以及仓库内的 `third_party/level-zero-ext` 子模块。

## 构建

1. 克隆并拉取子模块：

   ```
   git clone --recursive https://github.com/apkipa/AGShot.git
   ```

2. 准备 OCR 模型，放到仓库根目录的 `models/`（见下一节）。

3. 用 Visual Studio 2022 打开 `AGShot.sln`，选择 x64 配置构建；也可以直接调用：

   ```
   msbuild AGShot.sln /p:Configuration=Debug /p:Platform=x64
   ```

构建完成后，输出目录下的 `models` 是指向仓库根目录 `models/` 的目录联接，程序启动时从那里加载模型。

## OCR 模型

模型不随源码提交，需要放在 `models/` 目录中，包含以下三个文件：

- `ppocrv6_small_det_u8_template.ngraph_lite`
- `ppocrv6_small_rec_u8_template.ngraph_lite`
- `ppocr_keys_v1.txt`

程序在可执行文件旁的 `models/` 中查找，找不到时再尝试当前工作目录。首次识别会把驱动编译出的硬件 blob 缓存到 `%APPDATA%\AGShot\cache`，后续启动直接复用。

## 使用

1. 启动 AGShot，按 F1（或配置的快捷键）开始截图。
2. 拖动鼠标选择区域；靠近窗口或控件时选区会自动吸附，按需微调。
3. 点击工具栏的 **Copy to clipboard** 或按 Ctrl+C 复制选区图像；点击 **Recognize text** 进入 OCR 视图。
4. 在 OCR 视图中拖动选择文字，Ctrl+C 复制并关闭，Esc 取消。

## 配置

配置文件默认位于 `%APPDATA%\AGShot\config.toml`。若可执行文件旁存在 `AGShot.portable` 文件，则使用同目录下的 `config.toml`，便于便携使用。

```toml
version = 1

[window]
width = 800
height = 450

[screenshot]
hotkey = "F1"

[debug]
showStats = false
```

## 致谢

- [SnowShot](https://github.com/mg-chao/snow-apps)：交互与视觉上的参考。
