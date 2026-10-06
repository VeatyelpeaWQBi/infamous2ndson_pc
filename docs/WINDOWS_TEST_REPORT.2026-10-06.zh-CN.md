# Windows 构建与测试报告

日期：2026-10-06。范围：Windows x86-64 开发环境、原生构建及合成测试；不是 Second Son 游戏兼容性验收。

## 结果

| 项目 | 最终结果 |
|---|---|
| `build.bat --check` | 成功，全部工具和三个子模块路径存在 |
| `build.bat --build-tests` | 成功，主程序、GPU 能力程序及全部测试程序已生成 |
| Python/原生程序用例 | 80 项通过；失败 0、错误 0、跳过 0、原生用例排除 0 |
| 独立原生程序 | pad-test、file-mods-test 均通过 |
| CTest 单元检查 | 8/8 通过；包含四个 SPIR-V 验证用例 |
| Vulkan 集成测试 | 3/3 通过：场景分辨率、TAA、相机运动 |
| 最终正式入口 | `test.bat --gpu` 返回 0 |
| 实际游戏 | 未启动，Second Son 兼容性未验证 |

最终 Python 套件耗时 6.979 秒，CTest unit 组 0.24 秒，GPU 组 3.60 秒。这些是测试耗时，不是游戏性能指标。

80 项中包含此前因缺少程序而排除的 28 项原生用例。Linux/AppImage 的历史模块 `test_packaged_vulkan` 按项目平台原则明确排除，不属于 Windows 测试范围。未把重复运行计为额外独立用例。

GPU 测试实际完成多种分辨率的颜色/深度/模板往返、TAA 历史有效性与遮挡处理、相机运动等断言；场景测试的颜色和深度格式均满足条件，没有走“不支持则返回”的分支。

## 复用环境和安装结果

- 使用现有 Windows Python 3.12.10：`C:\Users\USER\AppData\Local\Programs\Python\Python312\python.exe`。
- 使用现有 MSYS2：`C:\msys64`。首次依赖安装成功；用户随后执行完整系统更新，确认没有待更新项。
- 使用现有系统 Git，构建子进程设置 `MSYS2_PATH_TYPE=inherit`，没有另装 MSYS2 Git或修改全局 PATH。
- CLANG64 软件包管理器自动解析并安装 153 个工具/库及间接依赖，初始下载约 365 MiB、声明安装占用约 2.49 GiB。间接依赖中也带入了 MSYS2 Python；本项目验证仍使用上述原有 Windows Python。

| 组件 | 当前软件包版本 |
|---|---|
| Clang / LLD | 22.1.8-3 |
| libc++ | 22.1.8-1 |
| CMake | 4.4.4-3 |
| Ninja | 1.13.2-1 |
| SDL3 | 3.4.18-1 |
| FFmpeg | 9.0.2-1 |

三个子模块已按主仓库指定提交准备：FSR-Vulkan `c64f093404125e960813f95f1996c14780f0dd69`、ImGui `f1cc2ae15e53a861a874c3034aae6798fde194ab`、LibAtrac9 `efca2e3af35562a09a9bb6deed90e45b4b824dc4`。FSR-Vulkan 应用了项目原有补丁。

Git 克隆额外依赖耗时较长，因此使用 GitHub 官方 codeload 固定版本归档，解压到 `out\dependencies`，构建配置自动复用这些源码。未改变依赖版本：

| 依赖 | 版本 | 官方归档 SHA-256 |
|---|---|---|
| magic_enum | v0.9.7 | `b403d3dad4ef542fdc3024fa37d3a6cedb4ad33c72e31b6d9bab89dcaf69edf7` |
| miniz | 3.1.0 | `09569fc19d060ac9f5999ba9356728c2494ebe6a24ac0eb0a6b6ae3d396cfea6` |
| xbyak | v7.24.2 | `5afccb2961576cd42c3d1e5370cb566838ee80140d3d43bdec0e31dcbd6510a9` |

哈希为官方归档下载后的本地计算值，用于记录本次输入；不是独立发布者签名验证。

## 本轮修复

1. 构建子进程继承已有工具路径，保证可复用系统 Git。
2. CMake 自动识别 `out\dependencies` 的本地依赖，避免切换编译器导致缓存重建后丢失源码位置。
3. 升频测试适配当前三参数接口，覆盖 DLSS 支持与 FSR 3.1 回退。
4. 加载器非法指令测试检查 Windows 的异常代码 `0xc000001d`、`guest+0x0` 和既有终止状态 139，替换 Linux 信号格式假设；生产异常处理未改动。
5. Vulkan 集成测试最初全部在初始化阶段崩溃。定位发现 Vulkan-Hpp 的 `DispatchLoaderBase` 在未定义 NDEBUG 时增加字段，测试与静态图形库因此产生 ABI 不一致。新增 `tests/vulkan_test.h`，只在包含 Vulkan 接口时匹配生产库布局，随后恢复测试断言。三项测试修复后全部通过，临时崩溃诊断已移除。

## 文件及占用

- 主程序：`out\bb-probe.exe`，111,522,816 字节。
- 辅助程序：`out\bb-gpu-capabilities.exe`。
- 最终完整日志：`out\windows-tests-final.log`。
- CTest 详细结果：`out\gpu\Testing\Temporary\LastTest.log`。
- 历史 2026-10-05 报告保留，其“缺少依赖、仅 52 项通过”描述只适用于当日。

实测当前 CLANG64 目录约 2.507 GiB、软件包缓存约 0.421 GiB、项目 out 约 0.652 GiB、子模块 Git 数据约 0.132 GiB、三个子模块工作文件约 40.31 MiB。它们包含原有内容和用户更新产生的缓存，不是精确的本轮磁盘增量。

## 边界

未更改原始游戏文件、正式存档、全局环境变量或系统服务。没有下载可选 DLSS/FSR 4 模型。主程序依赖现有 CLANG64 DLL 和启动脚本，尚未制作可脱离开发环境的便携发布包。

这些结果证明本机 Windows 构建和测试路径可用；不能证明 CUSA00309 已能进入游戏、画面正确、存档正常或达到目标帧率。启动器仍有血源默认值及专用补丁，下一阶段需要完成标题/指纹识别、补丁隔离及 Second Son 资源与接口适配。
