# 仓库内容与复现条件

目前可以克隆并审阅基础 XNU 补丁、状态文档及 A650 参数。仓库**还不是能单独重建最新实机移植的完整源码发布**，没有提供当前工作镜像对应的全部加载器、驱动集成、用户空间兼容改动、依赖锁定及镜像制作脚本。

```sh
git clone https://github.com/leafall903/ios-sweet-umi.git
cd ios-sweet-umi
git branch --show-current
```

默认分支为 `leafall903/umi-research-2026-10-25`。克隆用于阅读和源码研究，不构成安装步骤。

## 实验用过的组件

- 已解锁的 Xiaomi Mi 10 / umi，本次为 8 GB / 256 GB；不能推断其他面板或触控批次完全兼容。
- umi UEFI、DarwinBoot 衍生加载器、XNU 8796 / ios-sweet 路线与本机驱动适配。
- iPhone X iOS 16.7.16 / 20H392 官方输入，单独校验和提取；不在仓库内分发。
- 16 GiB HFSX 根卷；在这台实验机上使用 userdata 承载，而非无损并存 Android 应用数据。
- Mac 上的编译、镜像构建与本地检查；内核路线存在工具链和 IIG ABI 绑定，不能假设任意新版 Xcode 直接兼容。
- Fastboot 临时启动，以及 Recovery/TWRP 的只读日志回收；设备备份与原始日志保留在本地。

## 完整可复现发布还需补齐

1. 每个依赖的准确提交与适用许可证，当前 v17/v19 驱动改动的干净 diff。
2. 公开的构建与镜像打包入口，以及所有必需的外部输入说明；不分发 Apple 系统文件或厂商固件。
3. 启动配置的设备批次条件、根卷布局与恢复流程；在审阅前不发布可复制执行的刷 userdata 命令。
4. 与公开提交一一对应的脱敏启动日志、八核/输入证据及连续演示。
5. 可重复的帧时间和稳定性测量；A650 最小渲染通过后再发布加速结论。

`research/a650/UmiA650Platform.h` 是常量档案，不是 GPU 驱动。`xnu/patches/` 是继承的基础补丁集合，不应直接当成最新测试镜像的完整 patch series。
