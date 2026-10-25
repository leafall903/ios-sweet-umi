# 小米 10 / umi：iOS 16 适配研究

**维护者：[leafall903](https://github.com/leafall903)** · [English](README.en.md) · 状态更新：2026-10-25

在小米 10（`umi`，骁龙 865 / SM8250）上启动 iPhone 界面的 iOS 16，逐步适配存储、触控、多核与显示。实机已经进入锁屏、设置和 SpringBoard 界面，触控及实体按键能够工作；**当前仍使用 CPU framebuffer，Adreno 650 硬件加速没有完成，未达到稳定 60 fps。**

本仓库由 leafall903 独立维护。继承的 XNU 补丁保留原始作者与提交历史；它们与本次实机研究记录的关系见 [代码来源与贡献说明](docs/provenance.md)。本项目尚未获 HackiOS 官方收录。

## 阅读入口

- [实机状态与 A650 缺口](research/umi10-a650-status.md)
- [版本进展与性能证据](docs/bringup-history.md)
- [复现条件、仓库内容与发布限制](docs/reproduction.md)
- [代码来源、维护者与 GitHub 贡献者的区别](docs/provenance.md)
- [申请 HackiOS 官网收录：渠道、证据与英文介绍](docs/hackios-listing.md)

## 设备与系统目标

| 项目 | 本次研究配置 |
| --- | --- |
| 设备 | Xiaomi Mi 10 / 小米 10，代号 `umi` |
| SoC / GPU | Qualcomm SM8250 / Snapdragon 865，Adreno 650；Recovery KGSL 显示 `Adreno650v3` |
| 实验机 | 8 GB RAM / 256 GB 存储，Bootloader 已解锁 |
| CPU | 4 × Cortex-A55 + 4 × Cortex-A77，八核初始化及 SGI1 检查已在实机通过 |
| 屏幕 | 物理 1080 × 2340；研究中的 iPhone 逻辑画布 1125 × 2436 |
| 触控 | ST FTM5，I²C 地址 `0x49`；不能直接套用 Focaltech 或 LG V60 的触控协议 |
| 用户空间 | iPhone X / `iPhone10,3`，iOS 16.7.16，Build `20H392` |
| 内核路线 | ios-sweet / XNU 8796 系列；donor 系统版本与移植内核版本需要分别核对 |
| 系统盘 | 本次实验使用 16 GiB HFSX 根卷，放在手机 userdata 分区 |
| 启动 | 通过 Fastboot 临时启动 umi UEFI / 加载器，再交接 XNU 与 iOS 用户空间 |

**这套实机方案已经替换 Android userdata，不能称为保留原 Android 数据的无损双系统。** `fastboot boot` 临时加载启动镜像，不代表系统根卷也全部在 RAM 中，更不代表 userdata 没有被改写。

## 已做到、仍有问题、尚未验证

| 功能 | 结果 | 证据与限制 |
| --- | --- | --- |
| iPhone 锁屏、设置、SpringBoard | 已进入 | 多轮机主实际观察并操作；没有打包成公开 ROM |
| UFS / SMMU 存储接管 | 已验证读取并用于根卷启动 | SMMU apply、容量、GPT CRC、重复读校验完成，后续日志确认 HFS 根卷挂载与 launchd PID 1 |
| ST FTM5 触控 | 可操作，有过回退 | v9 本机身份适配后有响应；v13 加入有限事件恢复，v14 区分指纹区域释放事件；长期可靠性仍待测 |
| 电源 / 音量键 | 可操作 | 早期约 5–6 秒关屏迟滞及错色在后续版本明显改善；完整输入到显示延迟未测量 |
| 八核 | 已初始化并用于 GUI 候选 | 4 A55 + 4 A77、SGI1 PASS；不代表调频、功耗和持续负载均已完成验证 |
| 显示颜色 / 刷新 | 有改善 | v10 机主反馈基本消除原先的红白错色、逐行刷新；仍有合成异常 |
| 60 Hz 模式与呈现调度 | 已实现实验配置 | 配置为 60 Hz 不等于实际动画达到 60 fps；见下方测量 |
| 壁纸 | 未解决 | 资源转换与兼容候选实机测试后，机主反馈没有明显改善 |
| 时钟 / 日历等小组件 | 未解决 | 位置空白或全黑；快照、扩展服务与合成路径尚未串通验证 |
| 透明、模糊与 vibrancy | 未解决 | 出现模糊方向异常、透明短暂出现后消失；纯色替代不算修复 |
| Adreno 650 GPU | 仅诊断阶段 | v18 读取 GPUCC；v19 短时 CX 探测。没有成功执行 GPU 渲染命令 |
| Metal / 完整 CoreAnimation GPU 路径 | 未实现 | 仍需 GPU 内核客户端、用户态渲染及离屏计算路径 |
| Wi-Fi、蜂窝、蓝牙、音频、摄像头、充电、Apple ID、App Store | 本次没有成功验证 | 不能从其他机型的 HackiOS 功能表推断本机可用 |

## 性能：已有改善，但仍未达到 60 fps

v13 的有效日志中，帧间隔中位数约 109 ms，对应当时 10 Hz 限速。v14 取消该上限并修正呈现通知与定时节拍，机主反馈明显更流畅。

v16 一次约 1 秒窗口记录到约 **52.22 次提交/秒、40.39 次呈现/秒**；v17 某窗口约 **33.46 次提交与呈现/秒**。两次操作负载不同，不能据峰值直接评判版本优劣。这些是驱动调用计数，**不是屏幕上独特动画帧的视觉测量，也不是连续滑动平均帧率**。v17 用户反馈稳定，但仍不跟手。

复制耗时中位数约 3 ms；最后一段复制时间不能代替完整应用绘制、合成、排队和触控延迟。下一步需要解决上游渲染和 A650 驱动链路，而不是仅提高一个刷新参数。

## 已定位的具体问题

- **触控身份不匹配：**旧条件只接受参考机固件/配置。本机 v9 使用独立核实的身份与重复读取一致性条件，没有重刷触控固件或校准区。
- **特殊事件打断手势：**指纹区域的释放事件曾被当成普通触点拒绝。v14 依据原生 FTS handler 区分该事件。
- **快速操作 panic：**v15 出现 `IOGMD: not wired for the IODMACommand`，定位到描述符读写未完整执行 prepare / complete。v16 修正生命周期后，本轮实机未复现；不等于所有场景永久稳定。
- **壁纸与合成服务退出：**诊断中出现 PosterBoard 的 `_imageForView:` 异常，以及 ScreenshotServicesService 的 Metal `setComputeFunction:` 中止。它们提示需要继续排查用户空间/合成路径，尚不能据此认定一个统一根因。

## A650 加速还缺什么

1. 按 SM8250 / umi 的实际供电、时钟、复位和固件要求启动 GMU，验证 HFI / TrustZone 相关路径。
2. 建立 GPU/GMU 的 SMMU 流路由、页表、缓存一致性、命令环和故障恢复，先完成最小原生渲染。
3. 接通 XNU GPU 客户端与用户空间 Mesa/freedreno、CoreAnimation 及相关 Metal 离屏路径，再验证壁纸、小组件和模糊。
4. 用持续滑动、可重复场景和帧时间测量验证稳定性与性能。

[平台档案](research/a650/)只包含地址、长度和芯片 ID。`power_on=1` 只是一项电源探测观察，不是 GPU 加速成功。Redwood 的 Adreno 642L 与本机 A650 存在芯片 ID、资源范围和固件差异，不能原样复制初始化序列。

## 仓库里实际有什么

```text
README.md / README.en.md       中文与英文总览
xnu/patches/                   继承的基础 XNU 补丁与辅助脚本
docs/                         本次适配记录、来源、复现边界与收录材料
research/umi10-a650-status.md  A650 实机诊断结果
research/a650/                非运行式的平台参数档案
```

这里公开了基础补丁历史及整理后的研究记录，**没有包含可独立重建最新 v17–v19 实机镜像的全部集成源码、依赖锁定和打包流程**。仅克隆本仓库不能声称已经重建测试中的完整移植。该差距也是后续发布工作的重点。

没有发布 Apple 系统文件、手机备份、A650 固件二进制、原始私有崩溃日志或一键刷机包。现阶段请从[复现说明](docs/reproduction.md)评估公开材料的用途。

## 维护、来源与参与

项目维护与本机研究联系人为 [leafall903](https://github.com/leafall903)；问题与实机反馈可提交到[本仓库 Issues](https://github.com/leafall903/ios-sweet-umi/issues)。请注明版本、机型、现象和可公开的日志片段，移除设备标识与个人数据。

基础代码来自 [ios-sweet-umi](https://github.com/myh982516/ios-sweet-umi) 等公开研究，另参考 [UniXNU-umi](https://github.com/reAAAq/UniXNU-umi)、[ios-sweet](https://github.com/dipa-studio/ios-sweet)、[HackiOS XNU](https://github.com/oosexgang/hackios-xnu-kernel) 和 [HackiOS userland](https://github.com/oosexgang/hackios-xnu-userland)。各代码原有作者与许可要求继续适用；本仓库没有给所有继承代码重新授予统一许可证。详见[来源说明](docs/provenance.md)。
