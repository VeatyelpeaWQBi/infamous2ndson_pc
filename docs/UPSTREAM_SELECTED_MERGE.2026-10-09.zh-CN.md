# 指定上级改动合入记录

按用户 2026-10-09 的明确要求，选择性合入以下三项源码改动，保留既有未提交适配。
没有整体合并上级分支，没有导入跨平台启动器重构。

## 配置默认值集中管理

来源：[dfa0dcfd](https://github.com/yumlevi/bloodborne_pc/commit/dfa0dcfd499561cb55bf736645e03c6dd17d10b1)。
`scripts/bbport_settings_table.inc` 提供 19 项 INI 默认值、10 项游戏效果。
引擎 Load 在读取文件之前通过同一解析逻辑加载表，再应用文件及环境覆盖；重复 Load 不保留缺失键的旧值。
Windows 安装器嵌入并读取这张表，隐藏两个 debug 效果，因此显示 8 项；本地 Windows 启动器也从表取缺省值。
缺失项、文件覆盖、环境覆盖和安装器嵌入资源均已验证。

本地 CUSA00309 标题指纹、游戏路径、禁用血源补丁、游戏繁体中文/港版设置及 FPS 开关覆盖保持有效。
`menu_language` 是引擎 INI 项，与游戏系统语言不同；本轮没有导入独立的俄语菜单翻译提交。
Linux 启动器未改动，符合仅开发 Windows 的项目规则。

## 用上级实现替换本地 CPU 上传读取实现

来源：[312ed755](https://github.com/yumlevi/bloodborne_pc/commit/312ed755f9ce3d015fa2909cbcd7064c22d4221f)。
已删除本地 `MemoryManager::CopyCpuUpload` 和 `runtime_memory_copy_cpu_upload`，全部相关上传/哈希调用迁移到 `CopySparseMemory`。
精确回读模式下，它使用上级 `runtime_memory_read_backing`，通过未受 GPU 保护的 backing 读取，空洞和保留区读为零。
其他回读模式沿用上级原始 guest-view 上传路径；并行复制仍走现有 CopySparseMemory 调度。
上级函数外保留本地地址溢出/空指针检查，避免无效输入破坏输出。

真正的游戏 CPU 读取仍必须取得 GPU 最新数据；粒子所需的精确回读、异步回读和页版本校验未删除。
这些功能与此次替换的 CPU 上传实现职责不同。
原保护页、空洞、溢出及输出字节回归用例已迁移到新接口并通过。

## 纹理回收的两个缺陷

来源：[d7a53a5b](https://github.com/yumlevi/bloodborne_pc/commit/d7a53a5bf835484648dbff427d3061b8aa7e68da)。
保留图像不再扣除驱逐额度；LRU 使用 `invoke_result_t` 正确识别 bool 回调并提前停止。
另合入保留 GPU-written tiled 图像数量和容量的报告。

回归测试直接检查生产 LRU 的 bool 停止、void 遍历和年龄截止。
Vulkan 用例直接调用生产 TextureCache 的注册/回收路径：32 个必须保留的 tiled 图像不会阻止后面两个可驱逐项回收。
图像只使用元数据，未读取游戏地址或分配其图像内容；测试依然需要 Vulkan 设备，因此标记为 GPU 测试。

## 验证

- 运行程序和测试构建成功：`out/reverse-engineering/selected-upstream-build-repair.log`。
- Windows 安装器仅编译，没有打开安装窗口或安装依赖；共享资源核对得到 19 个默认值、8 个可见效果。
- `selected-upstream-tests.log`：188 项 Python、32 项原生单元、17 项真实 Vulkan 测试全部通过，无失败、跳过。
- 历史 Linux/AppImage 打包测试按既有 Windows 规则排除。
- 实际游戏隔离回归两轮均正常退出，未发生启动死锁或闪退；输入校验通过，镜头动作和火焰/烟气主体已通过截图确认。
- 两轮静止/转镜头 FPS 为 29.71/30.37、30.24/32.50。此前相同场景记录为 34.95/39.46，存在性能回退迹象。
  记录时刻并非同一时间的 A/B，尚不能将下降归因于某一个提交。主要指标变化是游戏线程回读等待增加，上传工作线程等待没有明显增加。
  当前合并已验证兼容性与缺陷修复，未取得 FPS 提升；性能原因仍需后续定位。
- 原始报告：`selected-upstream-runtime*.json`、对应 F11 `-metrics.json`，位于 `out/reverse-engineering/`。

本轮所有游戏程序、资源、着色器包及正式存档保持只读。
工作期间检测到本地外部提交 `74e6a01`（`partally merge.`）已保存源码改动；助手未执行该 Git 提交，也未重复提交。
