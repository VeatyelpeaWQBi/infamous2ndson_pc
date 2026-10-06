# inFAMOUS Second Son：可行性评估与适配方案

日期：2026-10-06。目标：Windows x86-64、CUSA00309。分析基线：本 fork 的 Windows 改造提交 `58cddbb`，不合入尚未评估的上游更新。

## 结论

**具备继续实施适配的技术条件，建议进入启动基线验证阶段。当前不能承诺进入实际关卡、完整可玩或指定帧率。**

有利条件：游戏的主程序和两个随附模块可被现有 SELF 解析器读取；已有原生 libc/Fios2 链接路线；Windows 原生程序、运行时单元测试和 Vulkan 合成测试已通过；游戏使用的多个系统功能有现成实现候选；旧日志对应的图像类型问题已有官方协调修复可参考。

主要障碍：导入匹配仍依赖血源的局部编号；缺少直接 Videodec 入口；启动、补丁、资源和渲染流程包含血源假设；系统服务、原生模块的内部依赖及开放世界资源流送仍需实际验证。

本次只解析二进制/元数据、统计资源和读取旧日志。没有启动本项目运行游戏，没有生成或执行游戏内存镜像，没有修改原始游戏、正式存档或安装组件。用户提供的是编译后的 SELF/PRX 和资源文件，并非开发商的 C++ 工程源码。

## 1. 游戏文件证据

目录：`D:\game\shadPS4QtLauncher\games\CUSA00309`。

| 项目 | 实际结果 | 对适配的意义 |
|---|---|---|
| 标题 | inFAMOUS Second Son™ / CUSA00309 | 标题专用配置可以明确识别 |
| SFO APP_VER | 01.00 | 不能直接套用“1.07”补丁 |
| SFO VERSION | 01.07 | 与 APP_VER 不同；按 eboot 指纹建立首个受支持配置 |
| eboot.bin | 17,079,995 字节，x86-64 SELF，可解析必需分段 | 无需先解决额外解密问题；不代表已能执行 |
| eboot 入口 | 相对地址 `0x61dfd0` | 可用于后续受控入口诊断 |
| 主程序映射范围 | 133,846,143 字节，约 127.65 MiB | 在现有 512 MiB 准备器限制内 |
| libc.prx / libSceFios2.prx | 均存在并可解析 | 可复用游戏自带实现，降低 HLE 重写量 |
| libc `_init_env` | 已确认实现只有 `C3`，即 RET | 满足现有运行时启用的一个验证前提 |
| 主程序 TLS | 初始 4 字节，内存 48 字节 | 与当前主程序 TLS 能力范围相容，运行仍待验证 |
| libc / Fios2 TLS | 内存分别 1,168 / 64 字节 | 需要确认模块 ID 与线程初始化 |
| 重定位 | 主程序 1/7/8，PRX 另有 16 | 属于当前准备/链接代码已有处理路径；本轮未实际生成最终链接镜像 |
| FS:[0] 特征 | 主程序可执行段内找到 2,927 处字节序列 | 需要 Windows TEB/TLS 重写；数量不是完成指令边界验证的证明 |
| 资源根目录 | art/cache、art/movies、art/ui | 与血源 dvdroot_ps4 布局不同 |
| art 资源 | 1,803 个文件，27,493,050,417 字节，约 25.60 GiB | 主要是流送和文件接口适配问题 |
| 文件种类 | 1,579 个 .xpps、7 个 .psarc、159 个 .bsf 等 | 先让原游戏代码解析资源，不先重写全部资源格式 |
| PlayGo | manifest 声明 71 个 chunk，初始 28 个 | 服务必须正确处理此分块范围和任务列表 |

本次只统计文件、大小和少量文件头，没有逐项检查资源完整性。不能据此认定镜像没有缺失/损坏或已合并某个更新版本。

主程序 SHA-256：

```text
2d1ca79630d7bbe6fa29575f59aa7996d43071d74ac8e4f7d40137967d9039ab
```

该指纹应作为首个 Second Son 配置的准入条件；其它程序版本先拒绝应用地址补丁。

## 2. 导入接口审计

eboot.bin 引用 466 个导入符号；libc.prx 引用 84 个，Fios2 引用 81 个。合并模块/库身份去重后是 **564 个导入身份**，不是把三个数直接相加。

| 静态分类 | 数量 | 含义 |
|---|---:|---|
| 现有宿主登记候选直接匹配 | 2 | 发现匹配声明，尚未调用验证 |
| 图形/视频输出/事件队列登记候选 | 104 | GnmDriver 85、VideoOut 13、libkernel 事件接口 6 |
| 随附模块原生导出候选 | 162 | libc、Fios2 及已允许的 libc-internal 回退 |
| 已有宿主实现，但作用域/编号需适配 | 237 | 主要包括内存、线程、同步、文件、音频、输入和系统服务 |
| 当前扫描未找到登记候选 | 59 | 44 项由主程序引用，其余涉及随附模块 |
| 合计 | 564 | 分类是静态线索，不是兼容率或实际运行覆盖率 |

例如，游戏的 `sceKernelCreateSema` 导入名是 `188x57JYp0g#b#T`，当前注册表使用血源的局部编号。相同 NID 不能因为后缀不同就被当成一个全新功能，也不能直接去掉所有后缀、不检查库/模块和 ABI。

应把注册/解析改为显式校验 **NID、库名、库版本、模块身份、符号类型**，并记录允许的版本兼容规则。现有 GPU 查询也只按 NID 前缀匹配，需要纳入一致的身份审计。

59 项待确认接口的分布：

| 范围 | 数量 | 初始优先级 |
|---|---:|---|
| libkernel | 19 | 高：模块信息、内存及原生 libc 相关调用；部分是数据符号 |
| libSceVideodec | 6 | 高：创建、解码、删除、Flush、资源查询、Reset |
| libScePlayGo / DiscMap | 7 / 1 | 高：资源安装状态、任务列表和 Fios2 依赖 |
| libSceLibcInternalExt | 1 | 高：原生 libc 内部依赖需识别 |
| libSceErrorDialog | 4 | 中：错误处理状态机 |
| libSceAppContent | 1 | 中：内容状态 |
| libSceSaveDataDialog | 1 | 中：保存界面结果语义 |
| libScePad | 1 | 中：官方登记确认是 scePadSetLightBar |
| libSceSystemService | 2 | 中：系统参数/服务 |
| libSceHttp / NetCtl / NpManager / NpTrophy | 5 / 2 / 6 / 3 | 按实际调用顺序；离线错误和回调仍必须正确 |

“未找到登记候选”也不等于必须从零实现：可能存在可复用的别名、语义相同的宿主函数或官方 HLE 实现。237 个候选中也包含受限/简化的实现；匹配成功之后仍要检查参数、句柄、回调和错误返回。

## 3. 实际旧日志提供的障碍

日志：`C:\Users\USER\AppData\Roaming\shadPS4\log\shad_log.txt`，修改时间为 2026-06-18，对应 shadPS4 v0.16.0、提交 `5be3f0a`。它不是本项目当前版本的运行结果，不能用于断言今天仍在同一位置失败。

确认的证据：

- 记录的标题是 CUSA00309，App Version 为 01.00，与本次 SFO 读取一致。
- MovieDecoder 调用了 Videodec 的资源查询、创建和 Reset，随后有 Flush/删除。当前项目只携带 Videodec 的 video_utils，已有 AvPlayer 不等同于直接 Videodec 接口。
- 第 420、514、637、772、824、825 行报告 BC1/BC4/BC5 的 1D 图像格式不支持。
- PlayGo 被实际使用，GetToDoList 的条目数为 71。
- 有陀螺仪/加速度初始化错误；当前 pad 数据保持默认姿态，缺少真实运动传感器采样。实际喷漆等交互是否受阻，需进入关卡后验证。
- 音频使用 48 kHz、1024 帧、Float_8CH_Std 和手柄扬声器端口；AJM 注册 codec_type=1。现有音频格式及 ATRAC9 路线有相应支持基础，但声音正确性尚未验证。
- 有缺少 trophy 元数据的错误。不能把奖杯文件完整性与主体资源完整性混为一谈；离线奖杯路径需要给出正确错误与回调。
- 日志末尾持续编译着色器并记录 11:08 的运行时间，没有足够证据确定停止根因，不能把运行时间解释成完成了实际游玩。

## 4. 当前代码中需要先隔离的血源假设

1. `scripts/run_windows.py` 默认选择 CUSA03173，并调用血源导向的补丁流程。
2. `scripts/patches.py` 默认读取 Bloodborne.xml、版本 01.09，包含固定地址的分辨率/UI 和帧率改写。必须先建立标题/指纹准入，禁止 Second Son 进入这一路径。
3. `scripts/prepare.py` 的资源统计及 `scripts/mods.py` 的资源布局识别使用 dvdroot_ps4。文件挂载本身较通用；原始资源应继续只读。
4. `src/runtime.c` 及多个注册表使用 CUSA03173 的局部导入后缀；`runtime_symbol` 也依赖同一套名字表。
5. 相机常量识别假设远平面 3000、矩阵位于特定常量偏移；UI/电影识别也包含血源着色器哈希。不能视作 Second Son 的相机和渲染阶段。
6. 部分系统服务把对话框立即完成或返回简化离线状态，需要核对 Second Son 实际轮询/回调行为。
7. 血源存档声音修补已有标题白名单；它不应扩展到 CUSA00309。

源码依据主要在 `scripts/{run_windows,patches,prepare,mods,link_modules}.py`、`src/{runtime,runtime_services,runtime_audio,runtime_pad,runtime_savedata}.c` 和图形核心的 `vk_camera_motion.cpp`、`vk_temporal_upscaler.{h,cpp}`。

## 5. 可借用的官方修复

### 压缩纹理与图像类型

本项目的 image.cpp 将 Color1D/Color1DArray 转成 Vulkan e1D，与旧日志错误相符。官方 shadPS4 的 [f6cd16e / PR #5133](https://github.com/shadps4-emu/shadPS4/commit/f6cd16e85e10a50b7116f9898478d1efdb640a51) 在 2026-09-28 合入了图像类型兼容改造：1D 转成高度为 1 的 2D，同时修改图像视图、采样坐标和资源处理。

该提交涉及 14 个文件。它提供了明确的移植方向，但不是只把 `e1D` 改成 `e2D` 就能安全完成；需要处理坐标、数组切片、图像视图、尺寸查询和既有血源改动的交叉影响，再用 BC1/BC4/BC5 合成样例验证。

### 直接视频解码

官方 [Videodec 实现](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/videodec/videodec.cpp) 明确注册了游戏缺少的六个入口。可以评估移入必要的 ABI 结构、生命周期和 FFmpeg 解码逻辑；现有 FFmpeg 开发库已具备，不需要为此先安装另一套视频软件。

### 历史兼容报告的使用边界

[CUSA00309 官方兼容报告 #1317](https://github.com/shadps4-compatibility/shadps4-game-compatibility/issues/1317) 对应 2025-07-19、v0.10.0 的 Boots 状态和 BC1 1D 错误。它支持“该类障碍曾存在”，不能代表最新 shadPS4 或本 fork 的可玩状态。本方案以本地二进制、旧日志和具体官方代码为依据。

## 6. 开发顺序、验收条件与停止判断

| 阶段 | 修改范围 | 必须达到的验收条件 | 工作量预估 |
|---|---|---|---|
| P0 标题与指纹隔离 | Second Son profile、独立输出/用户目录、资源根目录、禁用血源补丁与图形特例 | 识别本次指纹；原始文件只读；不生成血源补丁；未知版本拒绝地址改写 | 1–2 工作日 |
| P1 导入与模块启动 | 身份解析、注册表、数据符号、native libc/Fios2、TLS | 合成跨编号/版本用例通过；输出实际解析清单；模块初始化及首个宿主调用可诊断 | 3–7 工作日 |
| P2 系统与媒体 | 实际触发的 kernel 缺口、Videodec、PlayGo 71 chunks、DiscMap、离线状态机、文件回调 | 首次受控运行通过初始化和片头；所有未实现调用明确停止；不通过一律返回成功掩盖缺口 | 1–2 周，按实际调用重估 |
| P3 图形与资源流送 | 图像类型兼容修复、资源/着色器问题、读回与同步 | BCn 合成检查通过；菜单可见可交互；新游戏进入首个场景，连续运行 10–15 分钟 | 2–6 周，条件估计 |
| P4 游戏流程与优化 | 独立存档、手柄/触摸/姿态、开放世界加载、性能采样 | 能保存重载；关键交互可完成；固定路线重复验证，报告 CPU/GPU/流送瓶颈 | 待 P3 通过后重估 |

这些是开发工作量估计，不能简单相加为发布日期。首次有效启动验证窗口建议控制在 **5–10 个工作日**；如果 P1 仍受到复杂的原生 libc 信号/线程行为阻挡，或 GPU 移植需要大范围重构，就暂停扩大功能范围，重新评估本项目承载路线与完整 shadPS4 运行时的取舍。

初始运行基线使用游戏原始帧率与原生渲染路径，不启用血源帧率/分辨率补丁，不先依赖 FSR/DLSS 或游戏专用运动矢量。性能优化以实际可操作场景及固定路线为前提。

## 7. 测试与产物

新增只读工具：`scripts/analyze_game.py`。复现本次审计：

```powershell
& 'C:\Users\USER\AppData\Local\Programs\Python\Python312\python.exe' scripts\analyze_game.py `
  'D:\game\shadPS4QtLauncher\games\CUSA00309' `
  --report out\infamous-analysis\game-audit.json `
  --gaps-csv docs\INFAMOUS_IMPORT_GAPS.2026-10-06.csv
```

仅使用 Python 标准库及项目已有 SELF 解析函数。实际执行成功，生成约 300 KiB 的审计 JSON 和 59 项接口 CSV。审计工具不调用 prepare/link 的写镜像流程、不应用补丁、不执行游戏。

下一阶段新增测试要覆盖：跨游戏导入编号和版本隔离、符号类型匹配、71 chunk 状态与错误、Videodec 生命周期/帧输出、BCn 1D/数组图像坐标与视图、Fios2 读写边界及回调、独立存档和输入状态。已有 80 用例、2 个原生程序及 11 项 CTest 检查继续作为回归基线，不将其通过等同于 Second Son 游戏验证。
