# HeliosView v1.0.0

Async I/O release — a shared asio context for background work and network I/O,
a TLS-capable HTTP client, and a new console demo.

## What's new

**Async (`HeliosViewCore/Async.h`)**
- One asio context shared by the library: thread pool + timers + sockets
- Timers: `timer` / `sleepAsync` / `wait` / `waitAsync` / `sleep`
- Socket I/O: `read` / `write` / `connect` / `accept`, in both callback and
  stdexec sender styles (`helios::use_sender`)
- `BasicSender` — a generic stdexec sender ported from HeliosExec (the
  completion functor is the only customization point)

**HttpClient (`HeliosViewCore/Http.h`)**
- `http::Client` on the shared pool: http/https GET and POST
- TLS via beast `ssl_stream` with peer verification against a CA bundle
  (OpenSSL 3.5.2 fetched automatically at configure time — no manual install)
- One exchange per request (`Connection: close`); pooling planned under the
  same API

**Dependencies**
- Standalone asio replaced by the boostorg superproject submodule
  (pinned boost-1.92.0); CMake selectively inits only the libs the build
  needs — no `--recursive` checkout
- stdexec pinned to `b783aac` (matches HeliosExec)

**Examples**
- `webview_demo`: new fetch handler + URL input (https by default)
- New `async_http_demo`: console example exercising pool timers/sockets and
  http/https/POST

**Requirements**: Windows 10/11, C++23 (the C API is usable from C99),
CMake ≥ 4.3. Zero third-party setup — submodules, OpenSSL, and the WebView2
SDK are all fetched and initialized automatically by CMake.

---

# HeliosView v1.0.0（中文）

异步 I/O 版本 —— 共享 asio 上下文支持后台任务与网络 I/O，新增支持 TLS 的
HTTP 客户端，以及一个新的控制台示例。

## 新增内容

**Async（`HeliosViewCore/Async.h`）**
- 库共享一个 asio 上下文：线程池 + 定时器 + 套接字
- 定时器：`timer` / `sleepAsync` / `wait` / `waitAsync` / `sleep`
- 套接字 I/O：`read` / `write` / `connect` / `accept`，回调与 stdexec sender
  两种风格（`helios::use_sender`）
- `BasicSender` —— 从 HeliosExec 移植的通用 stdexec sender（完成函数是唯一
  定制点）

**HttpClient（`HeliosViewCore/Http.h`）**
- `http::Client` 运行在共享线程池上：http/https GET 与 POST
- TLS 基于 beast `ssl_stream`，对 CA bundle 校验对端证书（配置时自动拉取
  OpenSSL 3.5.2，无需手动安装）
- 一次请求一次连接（`Connection: close`）；连接池计划在相同 API 下提供

**依赖**
- 独立 asio 替换为 boostorg 超工程子模块（pin boost-1.92.0）；CMake 配置时
  按需只初始化构建需要的库 —— 无需 `--recursive` checkout
- stdexec 固定到 `b783aac`（与 HeliosExec 一致）

**示例**
- `webview_demo`：新增 fetch 处理 + URL 输入框（默认 https）
- 新增 `async_http_demo`：控制台示例，演示池定时器 / 套接字与 http/https/POST

**环境要求**：Windows 10/11，C++23（C API 可被 C99 使用），CMake ≥ 4.3。
零依赖准备——submodule、OpenSSL、WebView2 SDK 全部由 CMake 自动拉取并初始化。
