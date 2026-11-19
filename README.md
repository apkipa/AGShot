# AGShot

一个 Windows 截图工具，使用 C++/Win32 与 Direct2D 绘制截图浮层，支持即时标注与基于 Intel NPU 的离线文字识别。

## 特性

- **区域截图与智能吸附**：全局快捷键唤起浮层（默认 `F1`），基于 UI Automation 自动识别窗口与控件边界，支持拖动框选与四边四角微调。
- **即时标注**：选中区域后弹出悬浮工具栏，支持图形（矩形/椭圆/菱形）、箭头与直线、画笔/荧光笔、内联文字、序号徽章、GPU 模糊/马赛克、对象多选对齐与撤销/重做，以及带取色器的拾色面板。
- **Intel NPU 离线 OCR**：基于 PaddleOCR（DBNet + CTC）通过 Level Zero 提交 Intel NPU 推理，后台线程池异步执行不阻塞界面；识别结果支持拖动选字、`Ctrl+A`、`Ctrl+C` 复制并关闭与滚轮滚动。
- **独立设置窗口**：托盘打开的 WebView2 设置界面，支持快捷键录制、性能 HUD 开关、OCR 模型包导入（按钮选择或直接拖入 `.zip` / `manifest.toml`）以及 NPU 内存卸载与缓存清理。
- **常驻托盘与热重载**：无主窗口常驻托盘，支持 `config.toml` 修改自动热重载；缺少 OCR 模型时静默启动，不阻塞截图与标注。

## 系统要求

- Windows 10/11（x64）；设置窗口需要 WebView2 Runtime（缺失时自动回退打开配置文件）。
- OCR 需要 Intel NPU（如 Core Ultra 系列）及随驱动安装的 Level Zero 运行时；无 NPU 或未导入模型时截图与标注仍可正常使用。
- 构建需要 Visual Studio 2022（`v143`）、vcpkg（见 `vcpkg.json`）、Node.js + pnpm（前端打包与图标烘焙）、Level Zero SDK 及 `third_party/level-zero-ext` 子模块。

## 构建

1. 克隆并拉取子模块：

   ```
   git clone --recursive https://github.com/apkipa/AGShot.git
   ```

2. （可选）准备 OCR 模型放到仓库根目录 `models/ocr/`，或运行后在设置窗口导入。
3. 用 Visual Studio 2022 打开 `AGShot.sln` 构建 x64 配置，或命令行调用：

   ```
   msbuild AGShot.sln /p:Configuration=Release /p:Platform=x64
   ```

## OCR 模型

模型不随源码提交，由 `manifest.toml`（记录 `pack_format = 1`、`pack_version`、文件名与 SHA-256）及三个资源文件组成：

- `ppocrv6_small_det_u8_template.ngraph_lite`
- `ppocrv6_small_rec_u8_template.ngraph_lite`
- `ppocr_keys_v1.txt`

可在设置窗口的 **OCR** 页通过 **Import...** 或直接拖入模型包 `.zip` / `manifest.toml` 导入（自动做 SHA-256 校验并热加载），也可手动放入 `models/ocr/`。查找顺序依次为数据目录（默认 `%APPDATA%\AGShot\models\ocr\`）、程序同目录与当前工作目录；首次推理会把驱动编译出的硬件 blob 缓存到数据目录 `cache/`。

## 使用

1. 启动 AGShot，按 `F1`（或自定义快捷键）开始截图。
2. 拖动选择区域或单击吸附窗口/控件；需要标注时在工具栏选用对应工具。
3. 点击 **Copy** 或按 `Ctrl+C` 复制选区图像；点击 **Recognize text** 进入 OCR 视图。
4. 在 OCR 视图中选字、`Ctrl+C` 复制并关闭，按 `Esc` 取消。

## 配置

配置文件默认位于 `%APPDATA%\AGShot\config.toml`。若程序同目录存在 `AGShot.portable` 或已有 `config.toml`，则切换为便携模式（配置、缓存与模型均保存在程序同目录）。

```toml
version = 1

[screenshot]
hotkey = "F1"

[debug]
showStats = false
```

## 致谢

- [SnowShot](https://github.com/mg-chao/snow-apps)：交互与视觉上的参考。
