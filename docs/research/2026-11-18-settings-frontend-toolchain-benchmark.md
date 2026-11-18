# AGShot 设置页面前端方案与体积调研

- **日期**：2026-11-18
- **目标场景**：单 EXE（`AGShot.exe --settings`）内嵌 WebView2 设置页，通过 `RCDATA`（`IDR_SETTINGS_HTML`）打包进资源段。
- **参照基线**：既有设置页合入前的 Release 主程序 `bin/x64/Release/AGShot.exe`（`728,576` 字节，约 `711.5 KB`）。

---

## 1. 测试环境与工具版本

- Node：`v22.23.1`
- 包管理器：`pnpm 10.28.0`
- 打包与语言工具：`vite 6.4.6`、`vite-plugin-singlefile 2.3.4`、`typescript 5.9.3`
- 框架版本：
  - Preact：`preact 10.29.8`（搭配 `@preact/preset-vite 2.10.6`）
  - Svelte 5：`svelte 5.58.0`（搭配 `@sveltejs/vite-plugin-svelte 5.1.1`）
- 压缩比参考：`.NET System.IO.Compression.GZipStream`（`CompressionLevel.SmallestSize`）。

---

## 2. 四组方案横向实测（功能完全对齐）

四组实现均包含完全一致的功能与 UI 细节：左侧栏 3 页签（General / OCR / Storage）及窄屏自适应、快捷键录制、Performance HUD 开关、OCR 状态与缓存控制、存储路径与打开按钮、悬浮覆盖滚动条、以及 Sonner 风格三维叠放 Toast。

| 方案 | 产物路径（相对 `AGShot/`） | 未压缩原文 (`RCDATA`) | GZip 最小化后 | 相对原 Release 占比（原文 / GZip） | Vite 构建耗时 | `pnpm` 安装包数 |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: |
| **无框架 Vanilla TS + Vite** | `frontend-vanilla/dist/index.html` | **27,952 B**（27.3 KB） | **8,447 B**（8.2 KB） | ~3.8% / ~1.2% | **92 ms** | **16** |
| **原生手写（未 minify）** | `ui/settings.html` | 42,310 B（41.3 KB） | 9,704 B（9.5 KB） | ~5.8% / ~1.3% | — | 0 |
| **Preact + Vite (`singlefile`)** | `frontend-preact/dist/index.html` | 38,124 B（37.2 KB） | 13,355 B（13.0 KB） | ~5.2% / ~1.8% | 138 ms | 88 |
| **Svelte 5 + Vite (`singlefile`)** | `frontend/dist/index.html` | 65,475 B（63.9 KB） | 23,257 B（22.7 KB） | ~9.0% / ~3.2% | 485 ms | 41 |

> 注：以上 4 组方案基于相同功能与 UI 进行了对照实测；最终选定方案的代码位于 `AGShot/frontend/`（Vanilla TS + Vite）。

---

## 3. 关键发现与对比分析

1. **手写未 minify 版在 711 KB Release 下占比不小，但 GZip 收益极大**
   - `42,310 B` 文本经 GZip 后降至 `9,704 B`（`22.9%`），比直接依赖文本正则做源码级 minify 更安全、省体积也更明显。
   - 若脱离 JS/CSS 词法/语法分析器手写正则去压缩，极易误伤正则表达式字面量、模板字符串、ASI 自动分号插入或 CSS `calc()` 空格。

2. **为什么 Svelte 5 在小页面下反而比 Preact 重**
   - Svelte 所说的“无运行时”是指**没有 Virtual DOM diffing 运行时**；Svelte 5 仍会向单文件产物中打入 `svelte/internal/client`（Signals 响应式引擎、effect 调度、事件委托、keyed `#each` 与模板挂载辅助逻辑）。
   - 在只有单个设置窗口、少量卡片的轻应用里，Svelte 5 这部分固定运行时起手体积高于 Preact 的极简 VDOM + Hooks；当页面与组件继续膨胀时，Svelte 的增量曲线才会逐步追近。

3. **Preact 依赖包数量偏多的原因**
   - 本次实测 `Preact + Vite` 拉取 `88` 个包主要来自 `@preact/preset-vite`（内部依赖 Babel 相关转译插件做预刷新）；若改用 Vite 内置的 `esbuild` 直接转译 Preact TSX，包数可大幅下降，最终单文件产物体积保持同量级。

4. **Vanilla TS + Vite 的定位**
   - 无需引入 UI 框架运行时即可实现源码分文件、`tsc --noEmit` 严格类型校验、JS/CSS 自动 minify 与 Vite HMR，最终体积最小（`27,952 B` 原文 / `8,447 B` GZip）。
   - 代价是需要手动维护局部 DOM 状态更新，当未来出现大量动态表格、复杂联动表单时会比声明式框架更繁琐。

---

## 4. 资源压缩方式与耗时实测

### 4.1 WebView2 自定义拦截层对 `Content-Encoding: gzip` 的实测验证

实测在 `OnWebResourceRequested` 返回 `CreateWebResourceResponse` 时附带响应头 `Content-Encoding: gzip` 并直接传入 GZip 流：
- **结论：不可行**。WebView2 的 `WebResourceRequested` 合成响应不会经过 HTTP 内容解码器还原 `gzip`，浏览器直接把压缩后的原始二进制字节当作 HTML 文本解析并显示乱码（UI Automation 验证 DOM 未生成设置界面元素）。
- 因此需要构建期压缩 + 宿主 C++ 运行时内存解压后再喂给 `IStream`。

### 4.2 Windows 系统 API (`<compressapi.h>` / `cabinet.dll`) 压缩率与耗时实测

经核对本机 Windows SDK `10.0.26100.0`：`compressapi.h` 可直接由第三方 Win32/C++ 程序链接使用；Windows 内置的 `libarchive` 仅提供 `System32\tar.exe` CLI 及内部 Shell 组件，SDK 中无公开的 `*archive*.h` 头文件。

对 `42,310 B` 的原生设置页 HTML 在同机进行 200 次循环压测（取单次平均耗时；含每次重新 `CreateDecompressor` + `Decompress` + `CloseDecompressor` 的完整生命周期耗时）：

| 算法 / 方案 | 压缩后体积 | 压缩率 | 单次压缩耗时 | 热句柄解压耗时 | 含句柄创建/销毁解压耗时 |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **`MSZIP`**（`<compressapi.h>`） | **`9,742 B`** | **`23.0%`** | `1.88 ms` | `0.10 ms` | **`0.10 ms`** |
| **`LZMS`**（`<compressapi.h>`） | `9,922 B` | `23.5%` | `5.30 ms` | `0.21 ms` | `0.21 ms` |
| **`XPRESS_HUFF`**（`<compressapi.h>`） | `13,102 B` | `31.0%` | `0.15 ms` | `0.05 ms` | `0.05 ms` |
| **`XPRESS`**（`<compressapi.h>`） | `15,906 B` | `37.6%` | `0.08 ms` | `0.03 ms` | `0.03 ms` |
| **`.NET GZip`（SmallestSize 对照）** | `9,704 B` | `22.9%` | `0.66 ms` | `0.07 ms` | `0.07 ms` |

- `MSZIP` 与 `GZip` 压缩率几乎持平（仅差 38 字节），运行时完整解压仅约 **`0.10 ms`**，相对 WebView2 初始化耗时可以忽略，且零第三方 C++ 库依赖。

---

## 5. 最终选定方案

- **前端构建**：**无框架 Vanilla TS + Vite (`vite-plugin-singlefile`) + `pnpm`**
  - 源码按模块化 TypeScript / CSS 维护，带严格类型检查、JS/CSS 自动 Minify 与热更新，产物原文最小（~27.3 KB）、依赖最轻（16 个 dev 包）。
- **资源打包与加载**：**MSBuild 增量构建 + Windows `<compressapi.h>` (`MSZIP`)**
  - MSBuild 在前端源码变更时自动执行 `vite build` 并通过 `cabinet.dll` 压缩为 `MSZIP` 嵌入 `RCDATA`；设置窗口启动时在 C++ 内存一次性解压后交给 WebView2。

---

## 6. C++ 侧 JSON 方案与 Release 二进制体积对比

在完成 `Vanilla TS + Vite + MSZIP`（`28,088 B -> 8,393 B`）与静态 `WebView2LoaderStatic.lib` 接入后，对三种 JSON 方案进行了 `Release|x64`（开启 `/GL` / `/LTCG`、`/OPT:REF`、`/OPT:ICF`）实测对比（基准为设置页合入前的 `AGShot.exe`：`728,576 B`）：

| JSON 方案 | Release `AGShot.exe` | 附带新 DLL | 总二进制（EXE + 新增 DLL） | `.text` | `.rdata` | `.pdata` |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **`Windows.Data.Json`（WinRT）** | `844,800 B`（825.0 KB） | 0 | **`844,800 B`** | `0x71000` (452 KB) | `0x40000` (256 KB) | `0x5000` |
| **`nlohmann-json`（`3.12.0`）** | `918,528 B`（897.0 KB） | 0 | `918,528 B` | `0x7F000` (508 KB) | `0x44000` (272 KB) | `0x6000` |
| **`yyjson`（`0.12.0`，默认 `x64-windows`）** | **`834,560 B`（815.0 KB）** | `yyjson.dll` (`243,712 B`) | `1,078,272 B`（~1,053 KB） | `0x6F000` (444 KB) | `0x40000` (256 KB) | `0x5000` |

**观察与选型结论**：
1. **`nlohmann-json` 较重的原因**：虽然开启了全程序 LTCG 优化，但调用 `parse()` / `dump()` / 初始化列表会把完整 RFC 8259 词法解析器、UTF-8 校验、浮点序列化、默认 `std::map` 红黑树与异常错误格式化变成静态可达代码（LTCG 函数数由 `7477` 升至 `8782`），比系统内置组件 `Windows.Data.Json` 多出 `73,728 B`（+72.0 KB）。
2. **`yyjson` 的动态库副作用**：vcpkg 默认 `x64-windows` triplet 将 `yyjson` 编译为 `yyjson.dll`（`243,712 B`）随同输出，破坏了单 EXE 分发形态；若要静态链接需切换静态 triplet 或直接源码合入。
3. **最终选择**：保留 **`Windows.Data.Json`（WinRT）** —— 零外部 JSON 依赖、保持严格单 EXE（`844,800 B`，相较改动前仅 `+116,224 B` / `+113.5 KB`）。


