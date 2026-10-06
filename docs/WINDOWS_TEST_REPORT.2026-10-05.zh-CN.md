# Windows 基础测试报告

日期：2026-10-05。测试对象为当前工作区的 Windows 环境改造，尚未提交的修改也包含在本轮测试中。

## 结果

**52 项 Python 基础测试全部通过；完整原生测试因缺少依赖和构建产物未执行。不能判定项目全功能测试通过。**

| 项目 | 结果 |
|---|---|
| Python 测试 | 52 项通过，失败 0，错误 0，跳过 0 |
| 需要原生程序的 Python 用例 | 28 项明确排除，未执行 |
| Windows `test.bat --python-only` 入口 | 已实际运行，退出码 0 |
| 一键入口复测 | 同样 52 项通过，测试耗时 5.558 秒 |
| 工具及源码路径检查 | 未通过：MSYS2 CLANG64 和三个子模块缺失 |
| 完整非 GPU 测试入口 | 退出码 1，执行前报告缺少必需测试程序 |
| C/C++ 单元测试 | 未构建、未执行 |
| Vulkan 集成测试 | 未执行 |
| 原始游戏启动和性能 | 未执行 |

## 已运行的范围

| 模块 | 通过数 | 验证内容 |
|---|---:|---|
| 内容参数 | 2 | SFO 参数、试用配置、非法参数拒绝 |
| 模块链接 | 7 | 导入身份及版本匹配、TLS 重定位、FS→GS 改写、越界拒绝 |
| 模组 | 13 | 资源合并、覆盖顺序、目录冲突、Windows junction 拒绝、原始资源保护、退出后清理 |
| 补丁 | 11 | 分辨率/UI 配置、补丁冲突、原始指令检查、外部补丁解析；仍是血源补丁逻辑 |
| SELF 解析 | 6 | 文件偏移、程序头、缺失段、截断数据、加密/压缩格式拒绝 |
| Windows 启动配置 | 5 | 重启后的分辨率重算、显式覆盖、动态分辨率选择、无需 Unix shell 工具 |
| 升频资源检查 | 3 | 模型文件完整性、输出分辨率档位、初始化文件大小 |
| Windows 工具入口 | 5 | 已有工具定位、缺失依赖报错、构建参数、路径含空格时的命令传递、批处理失败状态传递 |
| **合计** | **52** | |

测试使用人工构造的文件和替代启动端点。启动器测试中的“Starting”输出不代表真的启动了游戏或原生程序。图形能力检查在这些配置测试中使用替代返回值，未实际初始化 Vulkan。

Python 运行时复用本机已有的 `C:\Users\USER\AppData\Local\Programs\Python\Python312\python.exe`。先直接运行 Python 测试入口（52 项通过，5.548 秒），再通过 Windows 批处理入口复测，以验证默认 Python 定位、参数和退出码传递。两次运行覆盖的是同一组 52 项，不计为 104 项独立用例。

历史 Linux/AppImage 模块 `test_packaged_vulkan` 按项目 Windows 原则明确排除。

## 无法执行的范围及原因

未定位到以下工具：MSYS2 Bash、CLANG64 Clang/Clang++、LLD、CMake、Ninja、pkg-config。

以下子模块源码缺失：

- `gpu/third_party/fsr-vulkan`
- `gpu/third_party/imgui`
- `third_party/LibAtrac9`

完整测试入口检查发现缺少 10 个必需程序及 CTest 登记文件：

- `out` 下的 bb-probe、pad-test、runtime-test、file-mods-test、sema-test、content-test。
- `out/gpu` 下的 motion-history-test、ui-composition-test、upscaler-support-test、motion-shader-test。
- `out/gpu/CTestTestfile.cmake`。

因此加载器实际执行、Win32 运行时、线程/锁/信号量、虚拟手柄、文件 ABI 及 C++ 图形逻辑尚未得到原生运行验证。Vulkan 场景分辨率、TAA 和相机运动集成测试也未执行。

## 后续补齐验证

需先明确批准并准备 MSYS2 CLANG64、配套开发库及上述源码依赖。依赖清单见 `WINDOWS_INSTALL.zh-CN.md`。这些操作可能产生数 GB 工具、源码及构建缓存占用，当前未实测。

依赖齐备后执行：

```powershell
.\build.bat --build-tests
.\test.bat
# 另行执行需要真实 Vulkan 设备的集成测试：
.\test.bat --gpu
```

本轮没有安装、下载或升级依赖，没有启动原始游戏，没有修改正式游戏文件或存档，也没有修改全局环境变量。测试使用了少量临时文件和 Python 缓存；测试临时目录由测试框架清理。
