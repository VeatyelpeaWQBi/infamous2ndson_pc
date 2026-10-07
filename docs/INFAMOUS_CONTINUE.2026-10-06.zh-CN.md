# Continue 后闪退的兼容修正

目标：CUSA00309，Windows x86-64，2026-10-06。

## 故障与对照

用户 `out/CUSA00309/game-test.log`（18:30:47）记录 Xbox One Controller 正常连接。
Continue 后在 `bb:VkRecorder` 线程、NVIDIA `nvoglv64.dll` 内访问地址 0x130，
主程序调用栈经已有 LLVM 工具解析为 `vkCmdEndRendering`。
未根据驱动内的故障地址推断用户需要更换或升级驱动。

用现有 pad replay 在启动后 15 秒按一次 circle、15.5 秒释放，
`out/infamous-continue-before.log` 复现另一个宿主崩溃：
`bb:DrawRec` 线程在 `RecordChunk::Push` 访问空记录块地址 0x10。
调用链为 BufferCache::ObtainBuffer → ResolveVertexBuffers → DrawRecord → DrawPipe。
共享 Scheduler 的记录块会在 KickRecording 时被移动交给录制线程；
Record/ReserveRecordData/KickRecording 采用单生产者协议。
额外 DrawPipe 阶段使该协议在此游戏转换路径中失效，是当前兼容性排查的首要原因。

对照检查：

| 运行方式 | 实际结果 |
|---|---|
| 原配置，自动 Continue | 宿主崩溃，退出 139 |
| 关闭 Vulkan 录制线程 | 50 秒未崩溃 |
| 关闭额外 DrawPipe，保留 Vulkan 录制线程 | 60 秒未崩溃，实际帧已是剧情介绍 |

这支持禁用额外绘制生产者的局部兼容修正，不等于已经完整定位、修复共享 DrawPipe 的全部竞争条件。

## 实现范围

- `scripts/game_profiles.py` 为 infamous 明确设置 `BB_DRAW_PIPE=0`，覆盖继承的旧配置。
- `vk_rasterizer.cpp::DrawPipeWanted` 对 infamous 禁用该阶段，手动调用原生程序时也不能被继承的 `BB_DRAW_PIPE=1` 绕过。
  GPU 初始化会依据标题 CUSA00309 设置 infamous 配置。
- Vulkan 录制线程、着色器、绘制命令和游戏 Continue 行为保留；不是删除绘制或跳过片头。
- 当前采用一个绘制生产者，额外流水并行的性能收益暂不适用于该标题。性能影响尚未量化。
- `scripts/diagnose_infamous.py --continue-game` 使用已有虚拟输入回放复现该转换，不调用桌面自动化。
  输入文件约 100 字节，位于既有 out 诊断目录；原始游戏及正式用户存档不参与测试。

## 验证

构建：`build.bat --build-tests` 成功，记录 `out/infamous-continue-build.log`。
测试：`test.bat --gpu`，110 项 Python/原生、2 项独立原生、13 项 CTest 单元/着色器、
4 项 Vulkan 集成全部通过，无失败、错误、跳过。记录 `out/infamous-continue-tests.log`。
启动路由测试检查即使继承 `BB_DRAW_PIPE=1`，Second Son 子进程也收到 0。

实际复现命令：

```bat
scripts\windows_python.bat scripts\diagnose_infamous.py --gpu --continue-game --seconds 120 --log out/infamous-continue-fixed.log
```

诊断 124 只表示外部运行时限结束，不能当作游戏正常退出。
最终已构建版本运行 120 秒，15 秒按 Continue，随后剧情文字连续滚动，
运行末段开始转换到后续画面；退出 124，由外部时限结束。
`out/infamous-continue-fixed.log` 无 Host/Guest fault、Critical 或 STOP。
18:40:44 的实际 GPU 读回确认剧情文字，18:41:41 的末段图像为明亮模糊的过渡画面。
后续 3D 画面准确性、可操控关卡及 FPS 仍未验证，不能据此宣称完整可玩。

重复检查（60 秒）：刻意在准备配置之后向原生子进程传递 `BB_DRAW_PIPE=1`，
Vulkan 录制线程采用默认启用状态。原生标题保护仍禁用额外 DrawPipe，
按 Continue 后未再发生致命错误；末段读回仍为明亮模糊的过渡画面（18:43:31）。
重复运行读取的诊断存档为 Auto0，与前一轮 Auto1 不同，因此不把末段图像当成剧情文字再次出现的证据。
记录 `out/infamous-continue-repeat.log`，由外部时限结束，退出 124。
这验证了用户无需手动修改环境变量，旧设置也不能重新触发该优化路径。
