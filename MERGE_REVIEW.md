# 合并评测与整改计划：`feat/paint-canvas-api` → `master`

> **English abstract** — Merge-readiness review of the 7-commit `feat/paint-canvas-api` branch
> (canvas/Blend2D/D2D engines, retained UI framework, examples reorg, README overhaul).
> The branch builds and both test suites pass locally, and `master` is a direct ancestor, so
> the merge is mechanically trivial. It is **not mergeable yet**: there is one
> public-API-reachable undefined behaviour (cross-engine `draw_image`), the branch has no CI
> signal at all (no PR build, tests never registered with CTest), and the SDK packaging
> pipeline plus its documentation were broken by the examples reorg. This document is the
> remediation plan; sections are ordered so that §3 (blockers) can be fixed independently of
> §4/§5 (correctness, polish).

| 项目 | 值 |
|---|---|
| 评测对象 | 分支 `feat/paint-canvas-api` @ `3d4bbd2` |
| 基线 | `master` @ `bdfbaca`（`origin/master`，是 HEAD 的直接祖先） |
| 规模 | 7 commits / 70 files / +28028 −7135 |
| 评测日期 | 2026-09-17 |
| 结论 | **暂不建议合并**；解除 §3 四个阻断项后可合 |
| 远端状态 | 本地与 `origin/feat/paint-canvas-api` 同步 |

---

## 1. 结论

代码本身可用：能配置、能编译、示例可运行、两个测试套件全绿，`master` 是直接祖先所以可快进合并、无冲突风险。

但当前状态合并仍有 4 个阻断项：1 个公共 API 可达的 UB、以及 CI / 测试注册 / SDK 打包三个工程性缺口。**先做 §3，再合**；§4 的正确性问题建议同批修完（它们是同一个功能面刚写出来的缺陷，越晚修成本越高）；§5 可随 PR 一起清理。

---

## 2. 现状与验证证据

### 2.1 仓库状态

| 检查项 | 结果 |
|---|---|
| 分叉关系 | `git merge-base --is-ancestor master HEAD` → 0；`master..HEAD` = 7，`HEAD..master` = 0（可快进） |
| 远端 | 本地分支与 `origin/feat/paint-canvas-api` 完全同步 |
| 子模块 | `third_party/boost` HEAD = 固定的 `dcc8af7` ✓；`blend2d` = `58ca946`、`asmjit` = `dffd8b1`、`stdexec` = `b783aac` 已固定 |
| 工作区 | 唯一脏项是 `third_party/boost` 的 54 个嵌套子模块浅克隆残留 —— 只影响本地，不影响合并 |
| 版本 | `project(HeliosView VERSION 1.0.0)`；仓库**没有任何 tag**，尚无已发布 API 可破坏，breaking rename（`7a2e9d5 refactor(canvas)!`）可接受 |
| 遗留标记 | `src/`、`include/`、`tests/`、`examples/` 中 TODO/FIXME/HACK/XXX **零命中**；release 路径无 `assert` |

### 2.2 实测结果

| 验证 | 结果 |
|---|---|
| CMake configure | ✅ 通过（VS 2026 自带 cmake 4.3.1，8.0s；blend2d/asmjit JIT 目标正常生成） |
| `HeliosView_CanvasTest.exe` | ✅ **157 checks, 0 failed**（真实逐像素断言，headless） |
| `HeliosView_PresenterTest.exe` | ✅ **32 checks, 0 failed**（该 exe 于 05:12 重链，晚于最后一次头文件改动 05:11，覆盖终态） |
| 全新全量构建 | ⚠️ **未完成**：ninja 卡在 `[0/2] Re-checking globbed directories...`，根因是沙箱禁止子进程创建管道（同源报错：`sh.exe: *** fatal error - couldn't create signal pipe, Win32 error 5`），不是本分支的问题 |

> **遗留验证事项（合并前补做）**：在一台没有沙箱限制的机器上跑一次全新 Release 构建
> （`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build`），
> 确认 §4 修复后仍是 clean build；CI 侧见 B2 的修复项。

复现命令（本机路径按实际 VS 安装位置调整）：

```powershell
& "<VS>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S . -B out/build/x64-Debug
out\build\x64-Debug\bin\HeliosView_CanvasTest.exe     # 157 checks, 0 failed
out\build\x64-Debug\bin\HeliosView_PresenterTest.exe  # 32 checks, 0 failed
```

---

## 3. 阻断项（合并前必须修）

### B1 — 跨引擎 `draw_image` 类型混淆 → UB 【P0，已逐行核实】

`native_bitmap()` 返回的是**引擎私有**指针，但 GDI+ 与 Blend2D 两个引擎都直接 `static_cast`：

- `src/win32/heliosview_canvas_gdiplus.cpp:606`
  `Gdiplus::Bitmap* source = src_adapter ? static_cast<Gdiplus::Bitmap*>(src_adapter->native_bitmap()) : nullptr;`
  → 该引擎的 `native_bitmap()` 返回 `m_bitmap.get()`（`Gdiplus::Bitmap*`，见 `:251`）
- `src/heliosview_canvas_blend2d.cpp:637-638`
  `src_img = *static_cast<BLImage*>(src_adapter->native_bitmap());`
  → 该引擎的 `native_bitmap()` 返回 `&m_image`（`BLImage*`，见 `:249`）
- 核心层不校验引擎：`src/heliosview_canvas.cpp:1432` 把 `image->adapter.get()` 原样传入，
  只排除了 `image == painter->canvas`

因此 `Painter(GDI+ 画布).draw_image(Blend2D 画布)`（反向亦然）会把 `BLImage*` 当
`Gdiplus::Bitmap*` 解引用 → 野指针 / UB。D2D 引擎已经用 `dynamic_cast<CanvasAdapterImpl*>`
正确处理（`src/win32/heliosview_canvas_d2d.cpp:933`），本项照抄该模式即可。

**修复方案（任选其一，推荐 A）**

- **A. 引擎内自保**：GDI+ / Blend2D 的 `draw_image` 改为 `dynamic_cast` 到自己的
  `CanvasAdapterImpl`；转换失败就**退回**用 `src.pixels` + `src.format` 包装一张临时位图
  （两条现有代码路径里都已经有这段兜底逻辑，直接复用）。
- **B. 核心层拦截**：`heliosview_canvas.cpp` 的 `draw_image` 入口比较
  `image->adapter->engine_id()` 与 `painter->canvas->adapter->engine_id()`，不同引擎时
  只传 `pixels/format`（或直接返回 `HELIOSVIEW_ERROR_UNSUPPORTED` 并在头注释里写明）。
- 无论选哪种，都要在 `include/HeliosView/heliosview_canvas.h` 的 `draw_image` 注释里
  明确跨引擎语义。

**验收**：新增测试，用 `ENGINE_BLEND2D` 画布做源、`ENGINE_GDI_PLUS`（及反向、以及 D2D）
做目标各画一次，断言像素落在预期位置；当前测试只覆盖同引擎路径。

---

### B2 — 分支完全没有 CI 信号 【P0，工程风险】

两个独立缺口：

1. `.github/workflows/build.yml:7-10` 只在 `workflow_dispatch` 和 `v*` tag 触发，
   **没有 PR / branch 构建**。
2. 测试从未注册进 CTest：全仓无 `enable_testing` / `add_test`；`CMakeLists.txt:295-298`
   只 `add_subdirectory(tests)`，`tests/CMakeLists.txt` 只有两个 `add_executable`；
   `build.yml` 里也没有任何 `ctest` 步骤。

合并 2.8 万行而 master 分支零验证，等于把回归风险推到下一次发版。

**修复方案**

```yaml
# .github/workflows/build.yml
on:
  workflow_dispatch:
  pull_request:            # 新增：PR 也要构建
  push:
    branches: [ master ]   # 新增：合入后回归
    tags: [ 'v*' ]
```

```cmake
# CMakeLists.txt（顶层，project() 之后）
enable_testing()
```

```cmake
# tests/CMakeLists.txt（追加）
add_test(NAME canvas    COMMAND HeliosView_CanvasTest)
add_test(NAME presenter COMMAND HeliosView_PresenterTest)
```

```yaml
# build.yml，Build 之后、Upload 之前
      - name: Test
        run: ctest --test-dir build --output-on-failure
```

**注意**：`tests/presenter_test.cpp:88` 会创建真实窗口（`helios::Window win(...)`），
在 windows-latest 上通常可用但并非保证；若不稳，用 `set_tests_properties(presenter
PROPERTIES LABELS "requires-desktop")` 标记并允许在 CI 中跳过，不要因为一个 GUI 用例
让整个流水线红。

**验收**：开一个到 master 的 PR，Actions 自动跑构建 + `ctest`，两个用例被真实执行
（`ctest -N` 能看到 2 个 test）。

---

### B3 — SDK 打包流水线被 examples 重组改坏 【P0，tag 时爆】

`.github/workflows/build.yml:87`：

```powershell
Copy-Item examples\CMakeLists.txt, examples\*.cpp, examples\*.c -Destination (Join-Path $root "examples")
```

示例已移入 `examples/01_console_core/ … 06_c_api/`，两个通配符**匹配不到任何文件**；
PowerShell 对无匹配通配符不报错（静默继续），于是 SDK zip 的 `examples/` 里只有一份
CMakeLists.txt，而该 zip 自带的 README 恰恰教用户 `cmake -S examples` 构建
（`packaging/README.md:108-113`）→ 必然失败。

**修复方案**

```powershell
Copy-Item examples\* -Recurse -Destination (Join-Path $root "examples")
```

并同步核对 `build.yml:90` 的 `Copy-Item build\bin\*.exe`：由于
`HELIOSVIEW_BUILD_TESTS` 默认 ON（`CMakeLists.txt:295`）且测试产物也落在 `build/bin`，
`HeliosView_CanvasTest.exe` / `HeliosView_PresenterTest.exe` 会被塞进 SDK 的 `bin/`，
而 `packaging/README.md:17` 把该目录描述为"预编译 demo"。建议二选一：
CI 打包时用 `-DHELIOSVIEW_BUILD_TESTS=OFF` 配置，或把测试产物输出目录与 demo 分开。

**验收**：本地在 tag 流程等价状态下跑一遍 `Stage SDK package`，解压 zip 确认
`examples/0N_*/` 六个子目录都在，并按 `packaging/README.md` 的步骤实际构建成功一个 demo。

---

### B4 — 打包文档与实际产物脱节 【P0，随 tag 对外发布】

- `packaging/README.md:37-43` 与 `:162-166` 仍写
  `HeliosViewDemo.exe / HeliosViewWebViewDemo.exe / HeliosViewWebViewEventsDemo.exe /
  HeliosViewWindowDemo.exe / HeliosViewSystemDemo.exe / HeliosViewAsyncHttpDemo.exe /
  HeliosViewCDemo.exe`；真实目标是
  `HeliosView_ConsoleCoreDemo / WindowShellDemo / WebViewBridgeDemo / CanvasUiDemo /
  StudioHybridDemo / CApiDemo`（`examples/CMakeLists.txt:30-60`）。
  新增的 Canvas / UI / Studio 三个 demo 完全没被提及。
- `packaging/release-notes.md` 未改动，仍是上一个 "Async I/O release"，而
  `build.yml:119` 用它当 GitHub Release 正文 → 发布说明与内容不符。

**修复方案**：更新两份文档的英文/中文段落（`packaging/README.md` 是双语的，两处都要改）；
`release-notes.md` 重写为本次内容（canvas 引擎三选一、PixelView/BufferPresenter、保留式 UI、
示例重组、breaking：`Paint`→`Canvas`）。

**验收**：文档里出现的每个 exe / 头文件 / CMake 目标名都能在仓库里找到对应物
（可用一条 grep 脚本自检）。

---

## 4. 正确性问题（建议与阻断项同批修）

| # | 问题 | 位置 | 状态 |
|---|---|---|---|
| 1 | `heliosview_canvas_pixel_view` 无视 DIRECT_PIXELS 契约：`canvas_data` 检查了，它没检查 → Blend2D 非 premul 格式下把**过期缓冲**交给 `BufferPresenter` / host 呈现（Blend2D 只在 `sync_to_buffer()` 回写） | `src/heliosview_canvas.cpp:587-598` vs `:579` | 已核实 |
| 2 | 预乘像素被原样按 straight 呈现：`render_to_dc` 用 `biBitCount=32, BI_RGB` + `SetDIBitsToDevice`，alpha 被忽略 → 半透明像素发黑。应反预乘或改用 `AlphaBlend` | `src/win32/heliosview_presenter_win32.cpp:83-91, 96-119` | 已核实 |
| 3 | 格式表与编码器不一致：`hdr/pnm/pgm/ppm` 标为可写，`codec_encode` 实际全部拒绝 → `format_supported()` 撒谎，`canvas_save("x.pnm")` 通过前置检查后失败 | `src/heliosview_image_codec.cpp:76-80` vs `:473-478` | 已核实 |
| 4 | `resize` 失败后 `adapter == nullptr` 却照常返回，随后 `painter_begin` 无检查地解引用 | `src/heliosview_canvas.cpp:629-631` → `:813` | 待复核 |
| 5 | GDI+ 的 GRAY8 用 `PixelFormat8bppIndexed` 却从不 `SetPalette`（全仓只有这一处 8bpp） | `src/win32/heliosview_canvas_gdiplus.cpp:242, 265` | 待复核 |
| 6 | 共享状态无锁：Blend2D 字体缓存 `static std::unordered_map s_font_cache`（AUTO 默认引擎就会走到）；D2D 用 `D2D1_FACTORY_TYPE_SINGLE_THREADED` + 首次调用线程的 `CoInitializeEx`；而公共头明确承诺 per-canvas 可任意线程创建/绘制/销毁 | `heliosview_canvas_blend2d.cpp:158,162,192`；`d2d.cpp:52,72`；`include/HeliosViewCore/Canvas.h:82-85` | 已核实（锁缺失） |
| 7 | 保留式 UI：`HostUiBinding` 由 `hv_alloc` 分配后既不释放也不在 host 销毁时移除；`binding->root` 在 widget 树销毁后不清空 → 绘制回调写已释放内存；`heliosview_host_ui_get_root` 是永远返回 `nullptr` 的桩 | `src/heliosview_ui.cpp:249-263, 272-275`；`src/win32/heliosview_host_win32.cpp:288-306` | 待复核 |
| 8 | C ABI 边界可抛异常：`hv_alloc` 会 throw，`heliosview_ui_widget_create` / `_label_create` / `_button_create` / `hv_host_create_raw` / `new heliosview_buffer_presenter` 都没接住；`stride_for` 的 `int32` 会在超大尺寸下溢出；`heliosview_canvas_encode` 用 `static_cast<int>(bytes.size())` 返回字节数；`SetWindowSubclass` / `RegisterClassExW` / `SetDIBitsToDevice` 返回值未检查 | `heliosview_internal.h:43`；`canvas.cpp:155, 754`；`ui.cpp:89, 302, 405`；`host_win32.cpp:324`；`hidden_host_win32.cpp:63`；`presenter_win32.cpp:110, 188` | 待复核 |

**处理原则**

- #6 二选一：要么加锁（`std::mutex` 保护 `s_font_cache`、D2D 改多线程工厂 / 每线程缓存），
  要么把 `Canvas.h` 里的线程承诺缩回"同一画布不得跨线程"。**不要两头都不做**——头文件的
  承诺就是用户会依赖的契约。
- #8 的修法是加一层 C ABI 兜底：所有 `heliosview_*` 导出函数外包 `try { ... } catch (const
  std::exception&) { hv_fail(...); return ...; }`，并在 `stride_for` 里做溢出检查。
- #2 是"已经能跑但画错"的一类，最容易被漏掉，务必补一个**像素级**用例
  （现在 `presenter_test.cpp` 只断言返回值，且有两处 `CHECK(true, ...)` 空断言）。

---

## 5. 打磨项（P2，可随 PR 一起）

| 项 | 位置 | 说明 |
|---|---|---|
| 文档示例编译不过 | `README.md:519`、`README_zh-CN.md:513` | `heliosview_tray_create(win, "Tray Icon", NULL, NULL)` 传了 4 参；真实签名是 3 参（`include/HeliosView/heliosview_tray.h:56-58`） |
| 克隆指引自相矛盾 | `README.md:136`、`README_zh-CN.md:136` | `git clone --recurse-submodules` 与 `CMakeLists.txt:39-41`「绝不要递归初始化 Boost（约 160 个子模块）」冲突，应为 `git clone` + `git submodule update --init --depth 1` |
| 机器专用脚本 | `scripts/build.cmd:14-16` | 硬编码 `E:\Microsoft Visual Studio\18 insider\...`（本机无 E 盘，脚本必然 `exit /b 1`）；要么用 vswhere / `where cmake` 探测，要么删掉。顺带 `:11` 的示例目标名 `CanvasTest` 也不对（实际是 `HeliosView_CanvasTest`） |
| 一次性迁移脚本入库 | `scripts/rename_paint_to_canvas.py` | 自述 "One-shot refactor"，规则是全局文本替换，**重跑会破坏已改好的文本**；建议删除或移到明确标注的一次性目录 |
| vendored 依赖缺署名 | `third_party/stb/stb_image.h`、`stb_image_write.h`（9,712 行） | README 第三方表（`README.md:126-130`）只有 WebView2/Blend2D/asmjit/stdexec/Boost，补一行 stb |
| 架构门控过宽 | `CMakeLists.txt:257-259` | 只要指针 8 字节就强设 `BLEND2D_TARGET_ARCH=X86_64`，Windows ARM64 / macOS arm64 / Linux aarch64 会被错误定向；应按 `CMAKE_SYSTEM_PROCESSOR` 映射 |
| 未实现面 | `src/heliosview_canvas.cpp:1488-1541` | 九个 `window_canvas_*` 硬编码 `"window drawing is not implemented yet"`（`heliosview_canvas.h:680-682` 已如实说明，可接受）；但 `_engine` 返回 AUTO、`_is_double_buffered` 返回 0 却不记录错误，语义不一致 |
| 参数被忽略 | `src/win32/heliosview_presenter_win32.cpp:96` | `render_to_dc` 的 `clip_box` 形参未使用 |
| 测试质量 | `tests/presenter_test.cpp:121,132`；`:94-105` | 两处 `CHECK(true, ...)` 空断言；`resizeFired` / `paintFired` 采集了却从未校验 |
| 测试写盘 | `tests/canvas_test.cpp:807-814` | 往 `examples/out` 写 PNG（依赖 CWD 可写）；建议改为可配置输出目录或临时目录 |
| README 内部漂移 | `README.md:91, 548` | 功能表仍把 `helios::WebViewWindow` 当主推 API，而 `3d4bbd2` 已将其降级为别名（`README.md:287` 自己写对了） |
| 既有漂移（非本分支引入） | `README.md:126` vs `CMakeLists.txt:227`；`README.md:129` vs gitlink | WebView2 SDK 版本 `1.0.4129.50` vs `1.0.4181-prerelease`；stdexec 写 `758f41f4` 而实际固定在 `b783aac` |

---

## 6. 执行顺序与验收

### 里程碑

| 阶段 | 内容 | 出口条件 |
|---|---|---|
| **M1 解除阻断** | B1 + B2 + B3 + B4 | 跨引擎用例绿；PR 上 Actions 自动构建并跑 `ctest`（`ctest -N` 列出 2 个用例）；SDK zip 内含 `examples/0N_*/` 且能按 README 构建 |
| **M2 正确性** | §4 的 #1–#7（#8 可拆到 M3） | 每个修复都有对应测试：预乘呈现的像素断言、`pixel_view` 的 DIRECT_PIXELS 断言、`format_supported` 与实际编码一致、失败路径不崩 |
| **M3 打磨** | §4 #8 + §5 全部 | 文档示例可编译；`scripts/` 无机器专用路径；clean Release 构建在本机与 CI 均绿 |

### 合并前 checklist

- [ ] B1 跨引擎 `draw_image` 已修 + 新增跨引擎像素用例
- [ ] B2 `pull_request` 触发 + `enable_testing()` + `add_test` + CI 跑 `ctest`
- [ ] B3 `Copy-Item examples\* -Recurse`；测试 exe 不再进 SDK `bin/`
- [ ] B4 `packaging/README.md`（中英）与 `release-notes.md` 已更新
- [ ] §4 #1–#7 已修且有测试覆盖
- [ ] 全新 Release 构建（非本沙箱环境）通过
- [ ] `git status` 干净（不含 `third_party/boost` 的本地浅克隆残留）
- [ ] 合并后确认 Actions 在 master 上跑过一次绿

### 已知的非阻断观察

- `master` 是 HEAD 的直接祖先 → 可直接快进；若走 PR，请用 **squash 或 merge commit 均可**，
  但注意 `7a2e9d5` 已带 `!` 标记（breaking），发布说明需对应写明。
- 仓库没有任何 tag，因此本次 breaking rename 不会伤害已发布使用者；第一次发版前把
  `release-notes.md` 补全即可。

---

*本文档由合并前评测生成，作为后续整改的依据；每完成一项请在 §6 checklist 打勾并注明提交号。*
