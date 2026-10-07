# Second Son libc 初始化阻塞修复

日期：2026-10-06；Windows x86-64；目标 CUSA00309。

## 问题及依据

用户日志 `out/CUSA00309/game-test.log` 显示 libc.prx 初始化在
`NWtTN10cJzE#libSceLibcInternalExt:1#libSceLibcInternal:257#F`
（sceLibcHeapGetTraceInfo）停止，返回地址位于 libc +0x5b8e1。

实际模块 SHA-256：
`9e8096d917f986a63e2f829f01d18acac7828914a7a18e34fb616faf97cc18af`。
解析并使用已有 LLVM 工具检查调用位置；调用前复制的常量位于 +0x9fc60，
字段为 `(size=32, flag=1, getSegmentInfo=0, mask=NULL, table=NULL)`。
调用后指令读取结构偏移 12、16、24。

官方 [libc_internal_memory.cpp](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/libc_internal/libc_internal_memory.cpp)
提供同一 void 接口和结构布局。它交付可写的 mask、64 项状态表，关闭 segment 信息；
不通过返回成功码表达结果。

## 实现

`src/runtime_libc_internal.c` 提供 32 字节 ABI，保留 size/flag，设置 getSegmentInfo=0，
返回对齐、可写且在进程生命周期内有效的 mask 和状态表。重复查询不清空状态，
也不覆盖结构后面的扩展数据。当前不提供 segment 跟踪生产者。
空指针或短结构明确停止，不能伪装成功。沿用既有严格库/模块版本和符号类型匹配。

## 验证

- Windows 离线构建和全部测试成功；没有新增依赖、环境或权限授权。
- `test.bat --gpu`：96 项 Python/原生测试、2 项独立原生测试、13 项单元/着色器检查、
  4 项 Vulkan 集成测试全部通过，无失败或跳过。
- 新增测试检查结构边界、输入字段保持、可写 64 项表、重复调用状态持久、
  新版本扩展尾部保持、错误版本/类型拒绝，以及空指针/短结构停止。
- 构建记录：`out/infamous-heap-trace-build.log`；测试记录：`out/infamous-heap-trace-tests.log`。

真实诊断直接复用现有 `out/CUSA00309/boot-linked.bin`，CPU 模式、外部 15 秒限制，
数据隔离在 `out/windows-test-user/heap-trace-user`，没有重写原始游戏或正式存档。
无需生成另一份完整内存镜像，也未打开图形窗口。进程退出码 20 表示明确的未实现导入停止。
`out/infamous-heap-trace-native.log` 记录：

```text
Module 0 initializer returned 0
Module 1 initializer returned 0
Entering original x86-64 code at guest offset 0x61dfd0
STOP: first unsupported PS4 import: 1j3S3n-tTW4#libkernel:1#libkernel:257#F (index 288)
```

模块 0 为 libc.prx，模块 1 为 libSceFios2.prx。以上是真实原生代码执行证据，
说明本次阻塞已解除，且这两个模块初始化完成。

新停止符号由本地官方 aerolib 对照确定为 `sceKernelGetTscFrequency`，调用返回位置为
游戏 +0x610639。该接口本轮未实现。尚未验证图形游戏进入菜单、关卡或可玩性。
