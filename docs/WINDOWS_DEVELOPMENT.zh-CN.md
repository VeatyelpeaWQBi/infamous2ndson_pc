# Windows 开发与验证流程

更新日期：2026-10-06。

本 fork 的开发与验收平台仅为 Windows 10/11 x86-64。MSYS2 CLANG64 用于构建 Windows 程序，不需要 WSL，也不维护 Linux/Unix 运行兼容性。完整开发原则见项目根目录 `AGENTS.md` 和 `agent.md`。

## 当前阶段与边界

本阶段已完成 Windows 构建、启动及测试基础设施验证。主程序及测试程序构建成功；80 项 Python/原生用例、2 个独立原生测试、8 项 CTest 单元检查和 3 项 Vulkan 集成测试全部通过。Second Son 游戏兼容性尚未实现或验证。详见 `WINDOWS_TEST_REPORT.2026-10-06.zh-CN.md`。

当前启动器和图形逻辑仍有血源专用假设，不能把 `run.bat --game-dir CUSA00309` 当作本阶段的游戏验收。下一阶段先实现标题/程序指纹识别、补丁隔离、资源目录适配，再验证 Second Son。

## 1. 复用已有环境

- Python：64 位 Windows Python 3.12 或更新版本。优先使用 `BB_PYTHON` 指定的现有程序，再使用 PATH 中的 `py -3` / `python`，最后才回退到已有的 MSYS2 Python。安装器不再默认安装第二套 Python。
- 编译器：已有的 MSYS2 CLANG64，配套 Clang、libc++、LLD、CMake、Ninja 和开发库。
- MSYS2 定位：优先 `BB_MSYS2`，否则从 PATH 中的 CLANG64 编译器定位，再尝试 `C:\msys64`。
- 运行时：保留 CLANG64 的 SDL3、FFmpeg、libc++ 等 DLL。启动器仅在当前子进程环境中加入其路径，不修改全局 PATH。
- 图形：现有显卡驱动提供的 Vulkan；不需要为了构建额外安装独立 Vulkan SDK。

若需要指定本机已有工具，可在当前 PowerShell 窗口设置：

```powershell
$env:BB_PYTHON = 'C:\Users\USER\AppData\Local\Programs\Python\Python312\python.exe'
# 只有实际已安装在该目录时才设置：
# $env:BB_MSYS2 = 'D:\existing-tools\msys64'
```

这些设置只影响当前窗口及其子进程。不要把它们当成要求新增环境变量或安装另一套工具。

## 2. 只读检查

在项目根目录执行：

```powershell
.\build.bat --check
```

检查现有 Python、MSYS2 工具和子模块源码路径。不会创建构建目录、下载依赖或启动游戏。

该检查只确认文件位置，不确认软件版本、所有开发包、链接能力或 GPU 初始化。缺失路径会返回非零状态。

本机已复用系统 Python 与 `C:\msys64`，CLANG64 工具/开发库、三个子模块及额外源码依赖已补齐，路径检查和实际构建均通过。

## 3. 无需编译的 Python 测试

```powershell
.\test.bat --python-only
```

使用标准库 unittest，测试 SELF 解析、模块链接、补丁、配置、Windows 启动流程与模组等。输入使用人工构造的小型文件，不读取原始游戏。

该模式明确排除需要原生 `.exe` 的用例，并输出排除数量；不是完整运行时验证。测试会使用少量系统临时文件和子进程，并在结束时清理临时目录。Python 可能生成少量 `__pycache__`。

Linux/AppImage 打包测试 `test_packaged_vulkan` 是明确排除的历史模块。其他意外跳过、导入错误或失败均使测试返回非零状态。

## 4. 构建 Windows 程序及测试

依赖和源码已存在时：

```powershell
.\build.bat --build-tests
```

不需要游戏文件。产物包括：

- `out\bb-probe.exe`、`out\bb-gpu-capabilities.exe`。
- `out\pad-test.exe`、`runtime-test.exe`、`file-mods-test.exe`、`sema-test.exe`、`content-test.exe`。
- `out\gpu` 下的 C++ 测试程序及 CTest 登记文件。

构建使用 Win32 源码和线程实现，不链接 pthread。显式保留测试断言，避免 RelWithDebInfo 的 NDEBUG 使断言失效。Windows PGO 暂时禁用，不读取血源/Linux 的 GCC 训练数据。

构建会创建或更新 `out` 下的目标文件、静态库、程序和缓存，并对 FSR-Vulkan 子模块应用项目已有补丁。本机实测 CLANG64 约 2.51 GiB、软件包缓存约 0.42 GiB、项目 `out` 约 0.65 GiB；源码与 Git 数据另计。

### 首次缺少源码依赖

普通构建会报错，不会自动下载或安装工具。如果用户明确同意网络和磁盘使用，可以执行：

```powershell
.\build.bat --allow-downloads --build-tests
```

该选项仅允许初始化缺失的 Git 子模块、获取缺失的 CMake 源码依赖；不会安装 MSYS2 或开发软件包。源码保存在现有子模块目录和 `out\gpu\_deps`。本机因 Git 下载缓慢，使用官方固定版本归档补齐 magic_enum、miniz、xbyak 到 `out\dependencies`；构建会自动复用该目录，或使用 `FETCHCONTENT_SOURCE_DIR_<NAME>` 指定已有源码。

安装开发软件包另见 `WINDOWS_INSTALL.zh-CN.md`。在图形安装器中选择 Install / Update 会明确开始工具/依赖下载，和普通构建的默认行为不同。

## 5. 完整的非 GPU 测试

构建成功后：

```powershell
.\test.bat
```

顺序执行 Python 测试、虚拟手柄和文件访问测试、CTest 的 unit 组。测试不启动游戏，不创建 Vulkan 设备。

缺少任一必需测试 `.exe` 或 CTest 登记文件时，入口直接失败，提示先构建。不会隐式构建、下载依赖，或把原生测试全部跳过之后报告完整成功。

运动着色器测试会在 `out\gpu\test-shaders` 生成少量 SPIR-V 文件。若已有 `spirv-val`，还会验证这些文件；否则 CMake 明确提示该验证不可用，不自行安装工具。其他测试覆盖仍可执行。

## 6. Vulkan 集成测试

```powershell
.\test.bat --gpu
```

先执行上述非 GPU 测试，再执行场景分辨率、TAA 和相机运动的 Vulkan 测试。需要真实可用的 Vulkan 设备及支持所用功能的驱动。使用人工构造的图像输入，不加载游戏。

这些测试验证部分真实 GPU 路径，不代表整份游戏画面正确、没有驱动问题，或达到某个 FPS。每项 GPU 测试有超时限制；失败时保留控制台输出。

## 7. 报告与阶段验收

当前已完成源码静态检查、Windows 原生构建、完整单元测试及 Vulkan 集成测试。详细记录见 `WINDOWS_TEST_REPORT.2026-10-06.zh-CN.md`。

后续验收应记录以下结果，不把未执行项标记为通过：

| 验证项 | 当前状态 |
|---|---|
| Windows 专用构建与代理规则 | 已写入源码和文档 |
| Windows 工具/子模块路径检查 | 全部通过 |
| Python/原生程序用例 | 80 项通过，0 失败/错误/跳过；原生用例未排除 |
| Windows 原生构建与测试 | 主程序及全部测试程序构建成功；2 个独立原生测试及 8 项 CTest 检查通过 |
| Vulkan 集成测试 | 3 项全部通过 |
| Second Son 游戏启动、操作与存档 | 后续游戏适配阶段，未验证 |

按项目环境规则，代理默认提供这些命令由用户执行；只有明确授权代执行后才运行构建、测试和游戏。
