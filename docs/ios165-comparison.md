# 16.5 完整对照与启动修复（2026-10-26）

本轮另建 iPhone X / iOS 16.5 / 20F66 用户空间，与可用的 16.7.16 / v23 对照。旧根卷及启动包分别保留。新的根卷完整复制 Apple 原版 OS、SystemOS 和 AppOS；组件通过 ZIP CRC、原 BuildManifest SHA384，普通文件逐个核对内容、权限和链接。Apple 文件和完整镜像不在本仓库发布。

32 个原版 staged apps 已使用原生目录移动安装至 `/Applications`。16.5 专用 Mesa、GL 桥接与容器兼容库独立构建；保留八核、FTM5 触控、64 位物理地址缓存修正、sRGB/BGRA 显示路径。键盘仅增加首次介绍页已显示标记。另移植了有实际 20F66 方法签名与指令检查的快照 PNG/JPEG 选择，以及媒体服务 portrait CoreImage 预热兼容。

目前不能宣布 16.5 桌面、小组件、壁纸或 60 fps 已成功。下列修复按启动阶段记录，避免把发送镜像、启动内核与完整系统运行混为一谈。

## 根卷标识随镜像变化

第一轮停在 `IOFindBSDRoot`：旧内核的 `umi_root_header_valid` 仍绑定旧根卷标识，新建 HFSX 的标识不同，因此根设备初始化返回错误。用真实新卷主/备卷头直接调用生产校验函数可复现；严格替换标识后，执行代码只有八条比较立即数变化。

修复保留 HFSX v5、4096 字节、16 GiB、双卷头、低位 DMA 与启用写入前的验证。旧 v23 内核保持原样。新的 `tools/make_root_volume_policy.py` 从指定完整 SHA256 的镜像读取主/备卷头，仅生成该镜像的私有单标识策略，避免换镜像后继续使用旧常量。生成结果应留在本地，不提交卷标识或系统文件。

第二轮已经显示 `OS version: 20F66`，证明越过原根卷识别问题。后续仍出现另一处 panic。

## 首次启动状态不能从完整复制中省略

第二轮调用栈的返回地址位于 `IOShutdownNotificationsTimedOut` 的 panic 调用之后。符号工具可能把返回地址显示成下一相邻函数 `IOPlatformExpert::start(...).cold.1`；必须检查前一条实际调用，不能据此误判平台初始化断言。

该超时是更早的重启请求的后续结果。上游曾记录 keybag 初始化失败导致同类现象。检查发现：已验证的旧初始根卷和 v23 都有 `/private/var/keybags/systembag.kb`；新的 16.5 初始根卷没有该目录或文件。ADT 已保留 `no-effaceable-storage`。旧构建器另外创建了 diagnostics、uuidtext、timesync 的迁移标记；新完整底座需要独立补齐这些实验平台状态。

`tools/umi_startup_seed.py` 使用与原方案相同的公开 `SWEET` 占位数据创建实验 KeyBag，并保留原日志服务 UID/GID 和权限。它不复制设备密钥、不覆盖未知已有 KeyBag，也不实现 SEP 或真实数据保护。文件内容和元数据须在离线新副本上写入、fsck 和只读回验后，才能更新设备。本轮设备验证结果后续补充。

## 可复用工具

- `tools/make_root_volume_policy.py`：完整来源哈希及双卷头检查后，只替换模板中唯一的卷标识常量。
- `tools/umi_startup_seed.py`：独立、可检查的首次启动计划；拒绝错误版本、路径逃逸和未知现存状态。
- `tests/test_bootstrap_tools.py`：实际生产函数测试，包括旧/坏卷头、双卷不一致、符号链接、未知 KeyBag 和错误版本拒绝。

来源参考：[ios-sweet 启动记录](https://github.com/dipa-studio/ios-sweet/blob/main/NOTES.md)、[Apple IOPlatformExpert.cpp](https://github.com/apple/darwin-xnu/blob/main/iokit/Kernel/IOPlatformExpert.cpp)、[HakiOS 已核对的用户空间版本](https://github.com/oosexgang/hackios-xnu-kernel/tree/0e6fb0bb83a5c71ce8887b8d66e28d41fef4a7e1)。
