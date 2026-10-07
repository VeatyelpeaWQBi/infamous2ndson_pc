# 香港地区 / 繁体中文启动配置

配置文件：项目根目录 `infamous-locale.json`。

## 启动前选择窗口

现在双击 `run-infamous.bat` 会先打开启动设置窗口。
可选 **繁體中文（香港） / English / 한국어（韩语）**，同时可选 ○（Xbox B）或 ×（Xbox A）确认。
窗口只列出当前游戏目录确实存在文字资源的语言；没有确认简体中文资源，因此不提供该选项。
香港地区和 UTC+8 保持固定，语音使用现有英文资源。
点击“启动游戏”才保存配置并继续启动；取消或关闭窗口不改配置，也不启动游戏。
下次打开会恢复已保存选择；仍可直接编辑下面的配置文件。

窗口复用现有 Windows Python 的 Tkinter，不安装任何 GUI 框架或工具。
沙箱内 Tcl 无法看见现有 Python 的 init.tcl，沙箱外隐藏窗口自测已通过。
正常用户账户启动不需要修改系统 PATH 或权限。
窗口用例验证资源过滤、取消不保存、启动保存并恢复选择、写入失败继续显示错误。
完整回归最终通过：126 项 Python/原生、2 项独立原生、14 项单元检查及 4 项 Vulkan 检查。
记录：`out/infamous-menu-tests.log`。首次完整检查有一个既有合成指令用例发生 5 秒超时，
同环境重跑未复现；窗口的四项自测两轮均通过。

## 默认配置

| 设置 | 当前值 |
|---|---|
| 地区标记 | HK |
| PS4 系统语言 | 10，繁体中文 |
| 时区 | UTC+8，480 分钟 |
| 确认键 | ○，Xbox B |

启动器会覆盖继承的英语/美区语言开关，并将这些值写入诊断会话 `manifest.json`。
运行时 SystemService 查询会得到语言 10、时区 480、确认键 0。
PlayGo 的语言掩码使用相同编号，即 `1 << 53`。
日期维持日/月/年，时间使用 24 小时制、不使用夏令时。

实际游戏元数据为 `CONTENT_ID=HP9000-CUSA00309_00-SECONDSONSHIP000`。
已确认存在 `art/cache/lang_chinese_text.xpps`（7,608,824 字节）和
`lang_chinese_text_p.xpps`（67,708 字节）；还存在英文和韩文文字资源。
列出的语言音频文件为英文；此配置不提供新的中文配音资源。
地区字段用于此次启动配置和诊断标记；离线运行时没有 PSN 国家/商店服务，
不会根据该字段生成账号地区、替换 DLC/奖杯身份或修改游戏 CONTENT_ID。

这次只编辑项目配置和对应运行时选项，未修改原始游戏文件或存档。
语言编号及确认键 ABI 依据
[OpenOrbis sys_service.h](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/_types/sys_service.h)。

修改后关闭旧进程，重新双击 `run-infamous.bat` 生效。
已验证启动路由、SystemService 输出、参数边界以及 PlayGo 语言掩码。
本次没有后台启动实际游戏；文字是否正确显示由用户下一轮测试确认。

构建/测试记录：`out/infamous-locale-build.log`、`out/infamous-locale-tests.log`。
Windows 构建成功；122 项 Python/原生、2 项独立原生、14 项单元/着色器/诊断检查、
4 项 Vulkan 集成测试全部通过，无失败、错误或跳过。
