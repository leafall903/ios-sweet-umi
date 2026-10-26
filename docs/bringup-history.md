# Current checkpoint: iOS 16.5 v6 (2026-10-26)

The published source checkpoint is now **iOS 16.5 / 20F66 v6**. The owner reported widgets normal in v5 and no remaining display errors in the v6 test. Smoothness, Maps, Date & Time and wider feature support remain incomplete; no sustained 60 fps result is claimed. See the [current README](../README.md), [16.5 progression](ios165-comparison.md) and [source/build boundaries](reproduction.md).

The material below is preserved as the **historical Chinese bring-up record**, including older 16.7.16/v17–v19 observations. Statements about missing wallpaper/widgets or probe-only A650 work describe those earlier checkpoints, not the current feature table.

---

# 实机适配进展与证据

状态整理于 2026-10-25。版本号是本次本地实验镜像标签，不是 GitHub release；它们不对应本仓库中每一个继承补丁的编号。

| 阶段 | 工作 / 观察 | 当前结论 |
| --- | --- | --- |
| 内核交接 | 高位 RAM 测试、加载对齐、可见日志与 UEFI 交接逐步验证 | 已越过早期内核启动，后续进入用户空间 |
| UFS / 根卷 | 按 umi 参考实现接管双 stream SMMU；容量、GPT CRC、重复读取及独立读回核对 | 存储读取有效，随后挂载 HFSX 并启动 launchd |
| v3–v6 | 用户空间缓存、控制台覆盖图形、时钟与启动身份属性排查 | v6 机主看到会刷新的锁屏；当时输入尚不可用 |
| v7–v9 | 本机 ST FTM5 身份与 HID 适配 | v9 触控有响应，但颜色及 5–6 秒按键响应延迟仍异常 |
| v10 | 显示 / 关屏路径调整 | 机主反馈逐行刷新及明显错色基本改善，仍很卡 |
| v11 | 四核 GUI | 比先前快，壁纸、小组件和透明仍异常 |
| v12–v13 | 触控停止原因排查、有限恢复；八核独立诊断及 GUI | 八核 PASS，GUI 有改善；仍不跟手 |
| v14 | 取消 10 Hz 限速、60 Hz 模式与节拍、通知参数、指纹释放事件处理 | 机主反馈明显流畅，但仍未到 60 Hz 观感；快速操作出现过代码画面 |
| v15–v16 | 符号定位描述符 panic，补齐 prepare / complete 生命周期 | v16 本轮快操作稳定，回收日志未发现相同 panic |
| v17 | 保留呈现周期相位，跳过错过的时隙 | 本轮稳定，机主仍反馈不跟手 |
| 壁纸/小组件候选 | 资源转换及容器服务兼容候选写入后测试 | 机主反馈没有明显改善；仍未解决 |
| v18 | A650 GPUCC 只读预检 | 进入原 iOS UI；没有启用 GPU |
| v19 | CX 电源域短时探测并恢复控制位 | 观察 `power_on=1` 后进入 iOS；没有证明 GPU 渲染或 GMU 启动 |

## 可公开的性能摘要

| 版本 | 样本 | 观察 | 不能据此得出的结论 |
| --- | --- | --- | --- |
| v13 | 26 条严格匹配帧记录 | 间隔中位数约 109 ms；复制约 3050 µs | 复制时间不等于完整帧耗时 |
| v16 | 一个 1015 ms 窗口 | 53 次提交、41 次呈现，即约 52.22 / 40.39 次每秒；合格复制样本中位数约 2977.5 µs | 不代表持续滑动平均值或独特动画帧率 |
| v17 | 一个 1016 ms 窗口 | 34 次提交、34 次呈现，即约 33.46 次每秒；复制样本中位数约 2943.5 µs | 操作不同，不能与 v16 的峰值直接横比 |

日志经暖重启回收，曾出现保留日志损坏或回收到旧 Linux 日志的情况。性能摘要仅采用完整且数值有界的记录；缺日志的轮次不算成功测量。raw crash、完整 RAM 日志和设备标识未公开，因此读者目前只能查看整理后的摘要，不能独立重算全部结果。

## 尚未公开的完整复现证据

最新集成源码、依赖固定、构建与打包脚本尚未完整发布；也没有公开连续实机演示视频。主机 ASan/UBSan 检查、对象比较和镜像回读曾用于本地检查，但它们不能代替真机驱动、硬件渲染与长期稳定性验证。本仓库的公开状态不应被描述为可复现的完整 A650 驱动发布。
