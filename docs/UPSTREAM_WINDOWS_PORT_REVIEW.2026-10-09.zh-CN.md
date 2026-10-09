# windows-port 上级差异审查（2026-10-09）

结论：不建议合并整个上级分支。建议单独移植最新的纹理缓存回收修复；精确回读上传死锁修复已有本地对应实现，不重复导入。
本轮只分析和检查补丁，未合并、未 cherry-pick、未修改渲染实现。

## 版本依据

- 指定上级：[yumlevi/bloodborne_pc windows-port](https://github.com/yumlevi/bloodborne_pc/tree/windows-port)。
- 本次 GitHub API 查询的分支头：`d7a53a5bf835484648dbff427d3061b8aa7e68da`，提交时间为北京时间 2026-10-06 13:43:47。
- 本地 HEAD：`07996346ea2ddb68993381bbbde2829fe3851486`，分支 `windows-port`。
- 本地已纳入的上级基线：`cb36414e2b83d6758516d1178fa54c9341242ede`。
- 上级在该基线后新增 5 个提交、涉及 25 个文件；本地在该基线后有 9 个提交，已提交差异涉及 191 个文件。
- 审查时另有 75 个未提交状态条目，包含此前的适配和本轮性能诊断，不能只对比 HEAD。

读取 GitHub 时，沙箱内 HTTPS 出现 TLS 认证失败；限定范围的只读 HTTPS 查询获准后成功。
没有修改全局环境、下载依赖、增加 remote 或执行 git fetch。

## 上级五项更新

| 提交 | 作用 | 本地建议 |
| --- | --- | --- |
| [dfa0dcf](https://github.com/yumlevi/bloodborne_pc/commit/dfa0dcfd499561cb55bf736645e03c6dd17d10b1) | 将游戏、安装器和启动器的设置默认值集中到同一表，增加菜单语言设置 | 可借鉴结构，非当前性能修复必需；需保留本地标题配置隔离 |
| [f5f5804](https://github.com/yumlevi/bloodborne_pc/commit/f5f58045a91c3257d42ad29d0cd88c95d949419e) | 英语/俄语引擎菜单翻译 | 当前无必要；不是 inFAMOUS 游戏语言/港版区域适配 |
| [71fee66](https://github.com/yumlevi/bloodborne_pc/commit/71fee661b9754f0827155fb3905f0027ef39cdb2) | 合并 Linux/Windows 启动器到 run_game.py，删除 run_windows.py | 不建议合并，目标与本地仅 Windows、CUSA00309 安全启动流程不同 |
| [312ed75](https://github.com/yumlevi/bloodborne_pc/commit/312ed755f9ce3d015fa2909cbcd7064c22d4221f) | 精确回读模式下，CPU 上传通过不受 GPU 保护的 backing 视图读取，避免工作线程/GPU 线程循环等待 | 本地已有等效目的、不同接口且带边界检查的实现，不直接覆盖 |
| [d7a53a5](https://github.com/yumlevi/bloodborne_pc/commit/d7a53a5bf835484648dbff427d3061b8aa7e68da) | 纹理回收只对实际驱逐扣预算，修复 LRU 回调提前停止判断，并增加保留图像统计 | 建议单独移植并补充行为测试 |

## 最新提交为什么值得移植

本地 `texture_cache.cpp` 的 `GarbageCollectImages()` 仍在判断图像是否必须保留之前执行 `--num_deletions`。
无法安全回写的 GPU 写入 tiled 图像会被保留，但仍消耗回收预算。足够多的此类老图像占住 LRU 开头时，
一次遍历可能没有驱逐任何图像就用尽预算。

本地 `common/lru_cache.h` 仍比较 `std::invoke_result<Func, ObjectType>` 与 `bool`。
前者是 trait 对象类型而非调用结果类型，判断恒为 false；应该使用 `std::invoke_result_t`。
因此返回布尔值的回调请求停止时，目前仍可能遍历所有旧项。

这两个缺陷都能从本地代码确认，属于通用缓存维护错误。修复保持必须保留的图像，不通过删除正确性所需数据换取帧率。
补丁只涉及 `lru_cache.h`、`texture_cache.cpp` 和 `texture_cache.h` 三个文件。
以 GitHub API 返回的真实补丁在内存中执行 `git apply --check --ignore-space-change`，退出码为 0。
这说明当前工作树的文本补丁可应用，不代表已完成编译、游戏或性能验证。

本次检查的近期正式/隔离运行日志没有匹配到纹理 memory-pressure 报告，
尚不能证明当前 30–40 FPS 的主要原因就是缓存回收。
上级提交中的 120 FPS 是其血源特定测试的报告，不能外推到本项目的 inFAMOUS。

移植后的验证应覆盖：保留项不扣预算、保留项后仍能驱逐、布尔回调能停止遍历、void 回调继续遍历；
再在隔离的低显存预算下检查回收与画面正确性，最后对照同存档实际 FPS/p95。
不要直接在用户正式游玩会话中改变显存预算。

## 与本地适配的差异及整分支合并风险

本地维护 CUSA00309 指纹校验、原始游戏只读、禁用血源地址补丁、港版/游戏语言配置、鼠标体感/触屏、
手柄异步震动、F10/F11 记录与崩溃恢复、隔离实际游戏测试，以及 Second Son 粒子/精确回读/页版本兼容逻辑。
上级这五项更新主要整理血源启动和菜单、修复通用 backing 上传和缓存回收，没有新增 Second Son 粒子或性能适配。

明确的结构冲突是上级删除 `scripts/run_windows.py`，而本地大幅修改了该文件且 `run.bat` 仍调用它。
新 `run_game.py` 默认指向 CUSA03173，仍调用血源补丁准备流程，未引用本地 `game_profiles.select_profile/native_environment`。
直接改入口会失去现有标题隔离与启动约束；必须专门移植，不能以文本无冲突代替兼容性判断。
配置表整理还触及 `bbport_settings.*`、构建、setup 和启动流程，需与本地菜单/设置逐项协调。

上级的 `runtime_memory_read_backing` 不应直接替换本地的 `runtime_memory_copy_cpu_upload`。
本地上传路径已经使用 backing，保留真正游戏读取的精确回读，并验证溢出、保护页、空洞及物理偏移。
本地的页版本回读、纹理别名修复和粒子兼容改动也应保留。

因此建议保持独立项目原则，仅选择最新缓存修复进行本地移植。尚未执行任何合并。
