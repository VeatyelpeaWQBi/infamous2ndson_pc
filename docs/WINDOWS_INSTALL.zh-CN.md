# Windows 安装、构建和运行指南

更新日期：2026-10-06；依据提交 `bbb7f72` 及本 fork 的 Windows 环境改造。命令行依赖安装、Windows 构建及测试已实际通过；图形安装器流程未实际验收。详细结果见 `WINDOWS_TEST_REPORT.2026-10-06.zh-CN.md`。

本 fork 现在仅开发 Windows 10/11 x86-64。新增构建与测试流程见 [Windows 开发与验证](WINDOWS_DEVELOPMENT.zh-CN.md)：优先复用系统 Python 3.12+，普通构建不下载依赖，首次源码下载需要显式选择 `--allow-downloads`。安装器仍保留血源游戏选项，不作为 Second Son 兼容性验收入口。

本目录虽然名为 `infamous2ndson_pc`，当前代码实际是 **Bloodborne 的 bbport**，重点适配 Bloodborne 1.09 / CUSA03173。安装这些依赖不会自动增加 inFamous: Second Son 的兼容性。

## 1. 最简单的方法：使用现有 setup.bat

**现有安装器已经包含安装依赖和构建项目的流程，无需再写一套安装脚本。**

`setup.bat` 先使用 Windows 的 .NET Framework C# 编译器生成 `out\bbport-setup.exe`，然后打开安装窗口。点击窗口中的 **Install / Update** 才开始完整安装与构建。

| 操作 | 作用 |
|---|---|
| 双击 `setup.bat` | 编译安装器并打开安装窗口 |
| `setup.bat --build-only` | 只编译安装器；不会构建 `bb-probe.exe` |
| Install / Update | 准备 MSYS2、安装依赖、下载子模块及所选模型、构建项目、写入设置和快捷方式 |
| Save settings | 保存设置和快捷方式；要求项目已经构建 |
| Play | 保存当前设置并通过启动器运行游戏 |

### 第一步：准备自己的游戏文件

准备已经合并 1.09 更新、程序段为明文 SELF 的 Bloodborne 游戏目录。例如：

```text
D:\games\Bloodborne\CUSA03173\
  eboot.bin
  sce_module\
    libc.prx
    libSceFios2.prx
  sce_sys\
    param.sfo
  dvdroot_ps4\
    ...完整游戏资源...
```

安装器不会下载游戏、解密 eboot、安装游戏的 1.09 更新或补齐缺失资源。`icon0.png` 用于生成快捷方式图标，不是构建必需文件。

安装器的游戏检查不完整：它会阻止选择没有 `eboot.bin` 的目录，但 SFO 缺失或版本不符时可能只显示警告，仍允许继续。请自行确认上述文件和版本。界面显示“OK”也不能证明所有资源完整、SELF 可解析或整个游戏兼容。

### 第二步：打开安装器

在资源管理器中进入：

```text
D:\projects\infamous2ndson_pc
```

双击 `setup.bat`。也可以在 Windows 命令提示符中执行：

```bat
cd /d D:\projects\infamous2ndson_pc
setup.bat
```

保持现有安装目录。当前路径没有空格，适合项目的构建脚本。在本地 Git 检出中启动时，安装器会构建现有代码、初始化子模块，不会自动切换到远程最新版主项目源码。

### 第三步：选择游戏目录和首次运行设置

在 Settings 页中：

1. **Game folder**：选择包含 `eboot.bin` 的游戏根目录，不要选择 `dvdroot_ps4`。
2. **Output resolution**：先选 `1920 x 1080`。
3. **Frame rate**：先选 `60 FPS`。
4. **Upscaler**：先选 `FSR 3.1`。
5. **Quality preset**：先选 `Native AA`。
6. **Live resolution changes**：选 `Off`。
7. 首次可以不勾选 DLSS、FSR 4 模型下载；这两种模型不属于基础运行必需项。
8. 按需要勾选桌面和开始菜单快捷方式。

以上设置用于建立首次启动基线，并不保证所有场景达到 60 FPS。安装成功并进入游戏后，再调整分辨率和升频方式。

### 第四步：点击 Install / Update

安装器依次执行：

1. 检查 `C:\msys64\usr\bin\bash.exe`；没有时下载安装 MSYS2。
2. 新安装的 MSYS2 执行两次系统更新命令。
3. 在 CLANG64 环境安装第 2 节列出的工具和库。
4. 在本项目初始化三个 Git 子模块及其递归依赖。
5. 下载你勾选的 DLSS / FSR 4 模型。
6. 执行 `bash build.sh`。CMake 可能额外获取 magic_enum、miniz、xbyak 源码。
7. 写入 `bbport.ini`、`Bloodborne.cmd`、`out\game_dir.txt`，并创建所选快捷方式。

首次需要联网访问 MSYS2 软件源、GitHub 和 raw.githubusercontent.com。可能出现 Windows 权限提示；按安装器显示的实际请求处理。常规游戏运行无需以管理员身份启动。

安装会产生 MSYS2 工具链、软件包缓存、子模块、构建文件和可选模型占用。FSR 4 的界面估计约 300 MB；其余总量没有在本机实测，可先按数 GB 工具与构建占用规划，并保留增长余量。游戏资源另计。点击 Install / Update 会实际开始这些操作，本文档的创建不会执行它们。

### 第五步：运行

安装窗口显示 **Done** 后，点击 **Play**，或双击项目内的 `Bloodborne.cmd` / 桌面快捷方式。

也可以直接启动：

```bat
cd /d D:\projects\infamous2ndson_pc
run.bat --game-dir "D:\games\Bloodborne\CUSA03173"
```

把示例游戏路径换成自己的真实路径。`Bloodborne.cmd` 会应用安装器保存的帧率、语言等启动变量；直接使用 `run.bat` 时，未提供的启动变量采用脚本默认值。

正常源码启动仍会调用构建脚本，更新未发生变化时会复用已有产物。`BB_PREBUILT=1` 可跳过构建，但仍需要兼容的运行库、Python、游戏文件和准备流程；它不是首次安装办法。

## 2. 完整依赖清单：当前 Windows 标准路径

### 系统和安装器

| 项目 | 用途 | 获取方式 / 当前电脑状态 |
|---|---|---|
| 64 位 Windows 10（项目声明 1803 起）或 Windows 11 | Win32 内存、线程和图形运行环境 | 当前 Windows 11 满足系统版本条件 |
| GPU 驱动与 Vulkan 运行环境 | 实际 Vulkan 渲染；项目要求 Vulkan 1.3 和必要扩展 | 当前 NVIDIA 616.56 / Vulkan 1.4.351 已通过已列出的基础条件查询 |
| .NET Framework 的 C# 编译器、Windows Forms、System.Drawing | 编译和运行图形安装器 | 当前 `Framework64\v4.0.30319\csc.exe` 存在；缺失时按安装器提示启用 .NET Framework 4.8 |
| MSYS2 x86-64 / CLANG64 | 提供本项目匹配的编译器、库和命令环境 | 本机已安装并更新，位于 `C:\msys64`；构建和测试通过 |
| 已有 Windows Python 3.12+（64 位） | 准备脚本、启动器和测试 | 优先复用系统 Python；安装器检查已有入口，不再默认安装 MSYS2 Python |
| Bash、pacman、coreutils、findutils、grep、sed 等基础命令 | 构建与下载脚本使用 | 由 MSYS2 基础环境及依赖提供 |
| Git、curl、Perl | 子模块、模型下载和 FSR 4 着色器优化 | Git 在安装器包清单中；curl / Perl 随 MSYS2 或包依赖提供，手动路径可显式安装 |

项目使用 MSYS2 **CLANG64** 的 LLVM / libc++ / LLD 组合。Git Bash、UCRT64 或 MINGW64 不等同于它。无需额外安装 Visual Studio、Visual C++ Build Tools、独立 Vulkan SDK、WSL、Docker、Java、Node.js、Maven 或数据库来完成这条标准流程。

MSYS2 官方说明：[环境区别](https://www.msys2.org/docs/environments/)、[安装器](https://www.msys2.org/docs/installer/)。

### CLANG64 软件包：安装器实际清单

下表后缀均对应 `mingw-w64-clang-x86_64-<后缀>`；`git` 是 MSYS 软件包，不加此前缀。

| 包后缀 | 用途 |
|---|---|
| `clang` | C / C++ 编译器；GPU 部分要求 C++23 |
| `lld` | 链接器，Windows 链接含 ThinLTO |
| `libc++` | 匹配的 C++ 标准库 |
| `cmake` | 构建配置，项目要求 3.24 或以上 |
| `ninja` | 执行 GPU 库构建 |
| `pkgconf` | 提供 pkg-config，定位开发库 |
| `sdl3` | 窗口、输入、音频等宿主功能 |
| `boost` | 视频核心使用的 Boost 头文件与设施 |
| `fmt` | 格式化库 |
| `glslang` | 将宿主 GLSL 着色器编译为 SPIR-V |
| `spirv-cross` | FSR 4 着色器转换和优化工具 |
| `spirv-headers` | SPIR-V 相关开发头文件 |
| `vulkan-headers` | Vulkan / Vulkan-Hpp 开发头文件 |
| `vulkan-loader` | 构建所需 Vulkan loader / 链接支持 |
| `vulkan-memory-allocator` | GPU 内存分配器头文件 |
| `xxhash` | 资源和缓存哈希 |
| `zydis` | x86 指令解码 |
| `robin-map` | 哈希容器 |
| `ffmpeg` | 视频播放相关开发库和运行库：avformat、avcodec、avutil、swscale、swresample |

软件包的间接依赖由 pacman 自动解析，包括编译工具的配套程序和媒体库依赖。无需逐个下载 DLL，也不要混用其他工具链的同名开发库。

你现有的系统 Python 3.12.10 可以直接复用。`run.bat`、`build.bat` 和 `test.bat` 使用统一的 Python 入口：`BB_PYTHON` 指定程序、PATH 中的 `py -3` / `python`、已有 MSYS2 Python，按此顺序定位。安装器不再把 Python 加入默认软件包清单。现有 `C:\devtools\ffmpeg\bin` 中的命令行程序仍不能替代 CLANG64 的 FFmpeg 开发库。

### 源码依赖

| 项目 | 位置 / 获取方式 |
|---|---|
| FSR-Vulkan | Git 子模块 `gpu/third_party/fsr-vulkan`；构建时应用 `gpu/patches/fsr-vulkan` 中的项目补丁 |
| Dear ImGui | Git 子模块 `gpu/third_party/imgui` |
| LibAtrac9 | Git 子模块 `third_party/LibAtrac9` |
| magic_enum | Windows CMake 找不到现有包时获取 `v0.9.7` |
| miniz | Windows CMake 找不到现有包时获取 `3.1.0` |
| xbyak | Windows CMake 找不到现有包时获取 `v7.24.2` |
| shadPS4 视频核心、着色器重编译器、sirit、GCN 和 half 等 | 本项目已携带的源码 |

三个子模块在当前检出中已按指定提交补齐，已验证 FSR-Vulkan 和相关代码参与 Windows 构建。magic_enum、miniz、xbyak 的固定版本源码也已补齐到 `out\dependencies`，由构建配置自动复用。首次在另一台电脑准备环境时仍需获取这些源码。

### 运行时必须保留的内容

- `out\bb-probe.exe`，以及启动流程使用的 `out\bb-gpu-capabilities.exe`。
- 本项目启动脚本、Python 准备脚本、补丁文件和所选游戏目录。
- 已有 Windows Python 3.12+ 和兼容的动态运行库，例如 libc++、SDL3、FFmpeg 等及其包依赖。标准启动器把 `clang64\bin` 加入当前进程 PATH，因此使用此安装方式后应保留 MSYS2。
- GPU 驱动和 Vulkan 运行环境。
- 可写的数据目录，用于 `out`、保存和 `bbport.ini`；默认在项目内。
- 使用 DLSS / FSR 4 时，对应的模型资产。

Windows GPU 库编译为静态库并链接进 exe，构建产物 `out\gpu\libbbgpu.a` 不会作为独立 GPU DLL 加载。其他依赖仍可能是动态库，不能只复制 `bb-probe.exe` 到另一台电脑。

### 可选功能

| 功能 | 额外内容 | 当前电脑适用性 |
|---|---|---|
| FSR 3.1 / TAA / Off | 无需单独下载 ML 模型 | 可作为首次启动配置 |
| DLSS | `out\nvngx_dlss.dll`；安装器模型选项或 `tools/fetch_dlss.sh` 下载 NVIDIA SDK v310.9.1 中的文件 | 当前 RTX 4090 Laptop 具备所需 GPU 扩展；NGX 初始化仍需运行验证 |
| FSR 4 v07 | `fsr4_shaders` 模型、权重和着色器，安装器界面估计约 300 MB；优化还用 spirv-cross、glslang 和 Perl | 已通过源码列出的硬件功能条件 |
| FSR 4.1.1 | 需要另外用 `tools/fsr4cap` 提取自己的 AMD DLL 资产；基础安装器不提供该流程 | 当前驱动缺少项目要求的 `VK_VALVE_shader_mixed_float_dot_product`，无需为首次运行准备 |
| 游戏调试菜单 | 对应调试字体文件 | 非必需；先不要启用 |
| Mod、外部补丁、手柄 | 对应资源或设备 | 非必需；键盘输入已有实现 |

## 3. 安装器的几个限制

1. **已有 MSYS2 不会在 EnsureMsys2 阶段自动完整更新。** 如果目录内已经有 `usr\bin\bash.exe`，这一步直接复用。旧 MSYS2 安装依赖失败时，请先按官方流程完整更新，再重试。MSYS2 只支持完整系统更新；参见 [更新说明](https://www.msys2.org/docs/updating/)。
2. **Install / Update 不等于更新主项目源码。** 当前目录已有 `build.sh` 时，它使用本地文件、初始化子模块；没有执行主项目 `git pull`。
3. **源码 ZIP 可能不含子模块。** 当前工作区有 Git 元数据，可以正常初始化；把不含子模块的 GitHub ZIP 当作完整源码可能构建失败。
4. **模型下载失败会中断本次安装。** 可先不选择模型，完成基础构建；之后再添加所需模型。
5. **没有自动完整验收。** Done 表示安装器各步骤返回成功，并不证明游戏能完整运行或通关。
6. **不是全局环境修复工具。** 它通过子进程的 `MSYSTEM=CLANG64` 和启动器 PATH 使用 MSYS2，无需手动改全局 PATH。MSYS2 安装程序本身会创建自己的安装文件和注册信息。

## 4. 已有 MSYS2，或 GUI 失败时的手动操作

以下是供你执行的命令，不会因阅读文档而自动运行。`pacman` 命令会安装或更新软件，Git 和构建命令可能联网下载源码。

### 4.1 准备或复用 MSYS2

已有 MSYS2 时复用原安装位置。没有时，可由 `setup.bat` 安装，也可从 [MSYS2 官网](https://www.msys2.org/) 安装到默认 `C:\msys64`。不要用 Git Bash 代替。

如果 MSYS2 位于其他位置，例如 `D:\devtools\msys64`，在打开安装器前于命令提示符执行：

```bat
cd /d D:\projects\infamous2ndson_pc
set "BB_MSYS2=D:\devtools\msys64"
setup.bat
```

这个 `set` 只影响当前命令窗口及其子进程。安装器会把非默认路径写入生成的 `Bloodborne.cmd`。

### 4.2 更新与安装包

打开 **MSYS2 CLANG64** 终端。先检查：

```bash
echo "$MSYSTEM"
```

应输出 `CLANG64`。然后按需进行完整更新：

```bash
pacman -Syu
```

如果提示关闭终端完成核心更新，按提示关闭所有 MSYS2 终端，重新打开 CLANG64，再执行 `pacman -Syu` 直到没有待更新项。不要同时运行多次 pacman。

安装完整工具与开发库：

```bash
pacman -S --needed git curl perl mingw-w64-clang-x86_64-{clang,lld,libc++,cmake,ninja,pkgconf,sdl3,boost,fmt,glslang,spirv-cross,spirv-headers,vulkan-headers,vulkan-loader,vulkan-memory-allocator,xxhash,zydis,robin-map,ffmpeg}
```

这个手动命令保留 pacman 确认提示，可在下载前查看实际下载量与安装占用。新增的 `curl perl` 明确满足模型下载及优化脚本需求。

### 4.3 由你检查工具与基础开发库

仍在 CLANG64 中：

```bash
clang --version
cmake --version
ninja --version
pkg-config --modversion vulkan sdl3 libxxhash libavformat libavcodec libavutil libswscale libswresample
```

这些命令检查工具和部分开发库；完整依赖解析仍以 CMake 构建结果为准。

### 4.4 下载子模块并构建

```bash
cd /d/projects/infamous2ndson_pc
git submodule update --init --recursive
BB_ALLOW_DOWNLOADS=1 bash build.sh
```

`BB_ALLOW_DOWNLOADS=1` 明确允许获取缺失的 CMake 源码库；普通 `bash build.sh` 不下载缺失依赖。两者都会应用项目的 FSR-Vulkan 补丁。第一次构建所需时间取决于网络、编译器和机器负载。在 PowerShell 中可使用 `build.bat --allow-downloads` 完成同一流程。

如果只想构建而暂时没有游戏文件，可以使用此手动路径；`build.sh` 本身不需要游戏。图形安装器则要求先选择含 `eboot.bin` 的目录。

成功后检查 `out\bb-probe.exe` 和 `out\bb-gpu-capabilities.exe`。GPU 编译失败详情在 `out\gpu-build.log`；CMake 配置失败的信息可能只在安装日志或终端中。

### 4.5 可选模型

仅下载实际准备使用的一种或两种模型：

```bash
# 使用 DLSS 时执行：
bash tools/fetch_dlss.sh

# 使用 FSR 4 v07 时执行：
bash tools/fetch_fsr4_assets.sh
```

模型下载失败不代表基础项目无法构建。使用 FSR 3.1 / TAA 可不运行这两条命令。

### 4.6 运行

回到 Windows 命令提示符：

```bat
cd /d D:\projects\infamous2ndson_pc
set "BB_FPS=60"
run.bat --game-dir "D:\games\Bloodborne\CUSA03173"
```

非默认 MSYS2 路径时，先设置对应的 `BB_MSYS2`。以上变量只作用于当前窗口。也可以重新打开 `setup.bat`，使用 Save settings 生成常用启动入口。

## 5. 常见问题

| 提示或现象 | 下一步 |
|---|---|
| 找不到 .NET Framework C# 编译器 | 按 setup.bat 的提示启用 .NET Framework 4.8；当前电脑已有对应 csc 文件 |
| No existing Python found / Python version error | 复用系统 64 位 Python 3.12+；可以在当前窗口用 `BB_PYTHON` 指定其完整路径，不需要额外安装 CLANG64 Python |
| No eboot.bin | 选择含 eboot.bin 的游戏根目录 |
| encrypted/compressed SELF segment is unsupported | 游戏程序文件不符合加载器的明文 SELF 条件；安装工具链不会改变该文件格式 |
| 缺少 libc.prx、libSceFios2.prx、param.sfo 或资源 | 补齐自己的游戏目录，不能通过安装开发库解决 |
| 找不到 sdl3 / vulkan / FFmpeg / fmt 等 | 确认正在使用 CLANG64，并已安装相应 CLANG64 开发包 |
| 下载包失败、签名错误或核心更新未完成 | 按 MSYS2 官方更新流程处理，完成后重试；保留错误信息 |
| GPU library build failed | 查看 Install log 和 out\gpu-build.log；停止于真正编译错误，不要把所有失败都归为模型问题 |
| 缺少 DLL | 通过 run.bat / Bloodborne.cmd 启动，并保留原 CLANG64 安装及其依赖 |
| DLSS 不可用 | 检查 nvngx_dlss.dll、实际使用的 NVIDIA GPU、驱动 NGX；安装器的 NVIDIA 识别依赖 NGXCore 注册信息 |
| FSR 4.1.1 不可用 | 当前本机驱动没有该路径要求的扩展；改用其他已支持选项 |
| 安装完成但游戏退出 | 保留控制台报错；构建成功、接口兼容和游戏数据完整性需要分别检查 |

本机已确认 Windows 11、i9-14900HX、约 64 GB RAM、RTX 4090 Laptop 约 16 GB 显存和正常系统 Python。现有 MSYS2 已由用户完成全面更新，工具/开发库、三个子模块和额外源码依赖均已补齐；Windows 构建、单元测试及 Vulkan 集成测试全部通过。CUSA00309 游戏目录已由用户提供，但 Second Son 的实际游戏适配与运行尚未验证。
