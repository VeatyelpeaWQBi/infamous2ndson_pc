# Second Son Windows 启动画面验证

后续：用户实际按 Continue 暴露了额外绘制流水线崩溃。
已增加标题专用顺序绘制配置；过程和实际验证见
[Continue 闪退修复记录](INFAMOUS_CONTINUE.2026-10-06.zh-CN.md)。
下文是标题画面验证时的历史状态；诊断 BMP 后续会被新画面覆盖。

2026-10-06；CUSA00309；Windows x86-64；RTX 4090 Laptop。

## 实际结果

使用现有游戏副本、已有 Python/MSYS2/Vulkan 依赖构建并运行。
真实游戏已显示 `inFAMOUS SECOND SON` 标志及 `○ Continue`，1920×1080。
证据：`out/infamous-presented.bmp` 是呈现链路的 GPU 像素读回，
60 秒检查末段在 18:17:20 读回；最终重新链接后的 30 秒重复启动在 18:20:19
再次读回同一标题画面（当前 BMP）。不是预置图片或仅有窗口创建的推断。
`out/infamous-open-gpu.log` 为 60 秒启动日志。诊断退出 124 表示外部计时终止，
不是游戏正常退出；本轮未出现 Guest fault、STOP 或 Critical。
此前 50 秒诊断同样未再发生致命错误。
最终重复启动日志为 `out/infamous-open-repeat.log`，同样由诊断时限结束，无致命错误。

继续按钮之后的菜单、片头、关卡、声音准确性、帧率及长期稳定性尚未验证。
仍存在 stencil test/op 值不同及图像层数修正警告，不能据此宣称完整兼容。

## 已修复的实际阻塞

- 补齐精确 NID/库名映射，使已有时钟和线程属性实现可被游戏调用；GPU 回退仍严格匹配版本和类型。
- Windows 线程指针重写扩展到 16 个目标寄存器；真实 GPU 启动使用 TLS 扩展槽，3616 处访问完成修正。
- 系统名称、安全显示区域、普通 PS4 模式、已安装资源 DiscMap 查询，以及离线网络句柄/超时/回调注册。
  无 PSN 登录、DLC 或奖杯注册数据时返回明确错误；没有制造联网成功或已解锁奖杯。
- `guest+0x8938f1` 的 SSE4a EXTRQ 通过 Windows 异常上下文计算兼容结果。
  同时覆盖立即数/寄存器形式、高位 XMM 编号、长度 0、标志保持；超出有效位范围的描述符采用移位/掩码结果。
- 图形录制数据超出固定 128 KiB 时按需分配足够空间，数据与消费命令保持同一生命周期。
- 中断写入产生的空图形缓存不再阻止下一次启动；新缓存先写完、关闭，再改名发布。
- `guest+0x1cfaaf` 文本顶点函数崩溃：原函数将输出指针存放在 RSP-32，属于 Windows 不保证保留的栈区域。
  为完整叶函数显式预留 80 字节，调整 28 个局部栈操作数、2 个栈参数访问及入口/唯一出口。
  所有原控制流和 RIP 数据地址保持原位；不跳过绘制、不删除检查。

## 地址修正的限制与依据

`scripts/infamous_cpu.py` 仅接受本机已审计的 eboot SHA-256：
`2d1ca79630d7bbe6fa29575f59aa7996d43071d74ac8e4f7d40137967d9039ab`。
链接前同时检查 TITLE_ID CUSA00309。函数范围 `[0x1cf4e0,0x1cfb4e)`，
原字节 SHA-256 `c7fb78d36b8e74b004fd2a350f625ce4c399cbb0483c87ca53b2a89eb74fbd54`。
不匹配则拒绝；只修改 out 内的准备镜像，追加一个 4 KiB 可执行段；游戏原文件只读。
该保护目前针对已观察到的函数，不代表全程序 red-zone 兼容已经解决。

[Microsoft x64 栈约定](https://learn.microsoft.com/en-us/cpp/build/stack-usage)
明确指出 RSP 以下的内存可能被系统或异常覆盖。
SSE4a 行为参考 [AMD 指令手册](https://www.amd.com/content/dam/amd/en/documents/processor-tech-docs/programmer-references/26568.pdf)。

## 验证与复现

- 构建入口：`build.bat --build-tests`；日志 `out/infamous-open-build.log`。
- 全部测试入口：`test.bat --gpu`；日志 `out/infamous-open-tests.log`。
  最终结果：110 项 Python/原生测试、2 项独立原生测试、13 项 CTest 单元/着色器验证、
  4 项实际 Vulkan 集成测试全部通过；失败、错误、跳过均为 0。
  EXTRQ 单项中包含 192 组寄存器/立即数/边界值组合。
- 限时启动：`scripts\windows_python.bat scripts\diagnose_infamous.py --gpu --seconds 60 --log out/infamous-open-gpu.log`。
- 用户入口：双击 `run-infamous.bat`；标题画面用手柄 ○ 或键盘左 Shift 继续。
  用户启动日志 `out/CUSA00309/game-test.log`；没有将开发诊断日志覆盖到该文件。

开发诊断使用 `out/infamous-diagnostic-profile`，每 60 个新游戏帧覆盖同一个约 5.94 MiB BMP；
对应 GPU 读回约 7.91 MiB，只在设置 BB_CAPTURE_FRAME 的诊断进程开启。
未改动正式存档、全局配置、系统 PATH、ACL；未下载或安装新依赖。
