# shadPS4 / AYOUB1080p 源码对照与选择性移植

## 固定来源和本地位置

用户于 2026-10-08 明确授权下载和对照这两个项目。本项目继续独立维护；本轮只移植相关实现，
不建立自动同步或整体合并关系。下载的是源码快照，没有运行其安装脚本，没有下载子模块或新增工具链。

| 来源 | 固定提交 | 本地目录（项目根目录下） |
|---|---|---|
| 官方 main | `0fe263a4760dfbfa973366890061749b4af0de97` | `out/reference-repos/shadPS4/shadps4-emu-shadPS4-0fe263a` |
| 官方 v.0.19.0 | `c7e065d1b415be16c23e260a21f1dd8bbfc4cb57` | `out/reference-repos/shadPS4-v0.19.0/shadps4-emu-shadPS4-c7e065d` |
| AYOUB1080p main | `153a5aa7f6b6be0e547d46719b793cb5145571d7` | `out/reference-repos/shadPS4-AYOUB1080p/weshayoub92-shadPS4-AYOUB1080p-153a5aa` |

AYOUB 提交的直接父提交是官方 `2338a06f923f1559a4c4b920ca26e029b2f4c07d`。改动清单保存在
`out/reference-repos/ayoub-commit.json`，75 个相关文件的本地映射对比在 `comparison.json` 和 `diffs/`。
三个源码快照及压缩包约 285 MiB；官方发布标签作为第三份快照用于准确核对用户提到的版本。
Git 直连先遇到沙箱证书错误，范围明确的网络授权通过后仍连接超时；GitHub API 压缩包下载成功。

## 开头喷漆与无体感手柄

官方 **v.0.19.0** 核心有鼠标体感，不必从 README 或 QtLauncher 界面寻找该实现：

- [input_handler.cpp 第177行](https://github.com/shadps4-emu/shadPS4/blob/v.0.19.0/src/input/input_handler.cpp#L177)：
  `hotkey_toggle_mouse_to_gyro` 默认映射到 `f6`。`GetInputConfigFile("global")` 将缺失热键补入核心用户目录
  `input_config/global.ini`，已有同名配置保留用户值。
- [input_handler.cpp 第777行](https://github.com/shadps4-emu/shadPS4/blob/v.0.19.0/src/input/input_handler.cpp#L777)：
  热键触发 `SDL_EVENT_MOUSE_TO_GYRO`。
- [sdl_window.cpp 第293行](https://github.com/shadps4-emu/shadPS4/blob/v.0.19.0/src/sdl_window.cpp#L293)：
  切换为 `MouseMode::Gyro`。
- [input_mouse.cpp 第89行](https://github.com/shadps4-emu/shadPS4/blob/v.0.19.0/src/input/input_mouse.cpp#L89)：
  `EmulateGyro` 读取鼠标相对位移，更新模拟加速度和陀螺仪数据。

这是源代码事实，不是对用户安装包实际版本、现有输入配置或喷漆关卡通过情况的验证。
没有传感器且没有启用替代输入时，等待体感动作是可能原因；真正画面冻结不能据此定性。
本地运行时已有 F6 鼠标体感、F7 居中、鼠标左键 R2，触屏模式使用 F8，不能把官方全部热键照搬到本地。

## 可以移植的修复与边界

| 实现 | 本地处理 | 目的与验收边界 |
|---|---|---|
| 多 GCN wave 工作组的 LDS 同步和更严格分支判断 | 从 AYOUB 选择性移植 SharedMemoryBarrierPass；保留本地阶段/资源结构 | NVIDIA wave32 执行 PS4 wave64 粒子排序等共享数据时避免缺失同步；CPU 和 GPU 行为回归验证 |
| legacy MUL/MAC/MAD 的零乘 Inf/NaN 规则 | 移植 AYOUB 实现 | 避免归一化、透明度等运算被 NaN 污染；不把这一潜在影响未经对照认定为全部火焰缺失根因 |
| GDS append/consume 的 lane 偏移 | 移植官方/AYOUB 的局部替换，额外修正中间 consume 表达式的用户选择 | 只修改目标使用点，保留其他共享 lane 计数用户 |
| 1D / 1D-array 图像原子坐标 | 移植官方 v.0.19.0 与 AYOUB 一致的实现 | 补齐本地已移植的 1D→2D 图像兼容逻辑 |
| 动态 S# 非归一化坐标位 | 移植 AYOUB 识别与后操作 | 正确识别 OR 0x8000 后的采样器语义 |
| 游戏线程等待异步回读 | 按 AYOUB 设计集成本地 BufferCache，CUSA00309 默认启用 `BB_ASYNC_READBACK=1` | GPU 命令线程继续提交；快照独立持有内存，按页拒绝旧数据，三次失败回退同步；最多复用 64 MiB 已完成的主机内存 |

着色器二进制缓存版本升到 8，使旧转译模块失效。游戏原始着色器包和可执行文件不修改。
重新编译可能增加首次进入新场景的耗时；后续性能测量需排除冷编译影响。

## 不直接整合的部分

- 欧/美版和港版是同一游戏，但其地址补丁仍与标题、版本、实际字节有关。
  未移植 CUSA00004 的 4K、灯光表、碰撞或码流补丁，没有改写原始游戏。
- AYOUB 的异步管线策略会跳过尚未编译的绘制。本地优先保证完整画面，不以跳过粒子换取帧率。
- WaveMemoryBarrierPass 的尾分支规则依赖退出 invocation 的驱动行为；未作为通用合法屏障策略移植。
- 第二回读队列、异步计算队列需要配套资源所有权和跨队列写入时序追踪，没有单独复制其队列初始化。
- Vulkan 录制线程、缓存、资源绑定复用等本地已有同类实现，避免整体替换破坏鼠标输入、F10/F11、画面截取
  和本地崩溃修复。AYOUB 缓存线程的退出逻辑也不能替换本地已有的排空/原子发布实现。
- AYOUB 文档的测试 CPU 是 9950X3D，默认分辨率补丁是 3200×1800；它不是本机稳定原生 4K 60fps 的实测保证。

## 验证记录

2026-10-09 更新：Windows 构建成功。171 项 Python 测试、31 项原生单元测试、15 项真实 GPU
测试通过，无跳过或排除。回归包括跨子组共享交换（128 invocation、32 次重复）、经真实翻译器生成的
legacy MUL/MAC/MAD 对零、负零、Inf、NaN 的 GPU 数值，以及 GDS 多使用点/consume 表达式。

用户已将游戏移动到 `D:\projects\infamous2ndson_pc\game\CUSA00309`。启动、语言菜单、诊断、性能测试、
着色器资源位置及记忆路径均已更新；新目录加入 Git 忽略。移动后的游戏指纹仍为
`2d1ca79630d7bbe6fa29575f59aa7996d43071d74ac8e4f7d40137967d9039ab`。
原始诊断记录中的旧路径保留为会话历史。

当前车祸现场存档，45 秒预热，静止/转镜头各测 10 秒，精确回读和完整主体效果：

| 同一程序的实验 | 静止 FPS | 转镜头 FPS | 说明 |
|---|---:|---:|---|
| 同步回读首次测试 | 21.25 | 25.61 | 缓存版本更新后，仍有少量编译，不能独立作为最终比较 |
| 异步回读、整个窗口作废 | 16.42 | 18.73 | 回读等待和字节数增加，发现一个 emitter 更新会作废其他 emitter 快照；未采用 |
| 异步回读、按页校验 | 25.39 | 28.51 | 只发布未变化页，重写页仍保护/重试；真实场景运行结束无故障 |
| 同一最终程序同步回读、已预热缓存复测 | 21.64 | 24.50 | 对照上行，收益分别约 17.3% / 16.4% |

报告位于 `out/reference-repos/port-game-{async-pages,sync-warm}.json`，对应日志同名 `.log`。
两轮原始会话由隔离测试按既有规则轮转保存；用户会话保持原状。图像已人工确认当前车祸场景的
火焰/浓烟主体，自动报告中的 `scene_verified` 未由脚本自行改为 true。

正常 Infamous 启动现在默认精确回读、4096 KiB 回读窗口、合并已知窗口、按页校验的异步回读，
以及已测的缓冲区/描述符复用；显式诊断环境覆盖仍保留。普通入口不默认跳过粒子或未编译绘制。

这些结果不等于稳定 50–60 FPS，也不是原生 4K 验收。此前缺少主体效果时的高帧率不能当作完整
渲染的性能基线。获得烟气能力的完整剧情、后续其他粒子及实体手柄震动仍需对应会话验证。
