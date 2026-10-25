# 维护者、贡献历史与代码来源

本仓库由 **[leafall903](https://github.com/leafall903)** 创建并维护。2026-10-25 的 GitHub 权限核查显示，仓库协作者名单只有 leafall903，角色为 admin。

## 为什么 GitHub 仍显示 myh982516

第一次发布使用 `myh982516/ios-sweet-umi` 的提交历史作为基础，基线为 `debeedb48c21b2622e85a3740160f77437ac403c`。新建仓库不是 GitHub fork，但完整父提交历史仍被推送了过来。GitHub 因此把 251 个历史提交归给 myh982516，把第一份新增研究文档提交归给 leafall903。

GitHub 的 Contributors 是提交作者统计，不是仓库管理权限，也不代表某人参与了这台手机的近期实测。改仓库名、分支名或 README 不会改变旧提交的作者。

旧提交保持原始作者与出处。后续本仓库的新增提交使用 leafall903 身份。这里不把继承补丁归为 leafall903 从零实现的成果，也不把原作者说成本项目的管理员。

## 各层来源

| 内容 | 来源 / 作用 | 本仓库的边界 |
| --- | --- | --- |
| `xnu/patches/` 基础历史 | [myh982516/ios-sweet-umi](https://github.com/myh982516/ios-sweet-umi)，ios-sweet 衍生研究 | 继承代码，不代表其中每一项都在本轮实机验证 |
| UFS / SMMU 接管参考 | [reAAAq/UniXNU-umi](https://github.com/reAAAq/UniXNU-umi) | 作为本机存储移植参考 |
| iOS 16 XNU 基础与接口参考 | [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet) | 不等同于现成 umi 安装包 |
| ST FTS 事件处理参考 | [LineageOS SM8250 FTS driver](https://github.com/LineageOS/android_kernel_xiaomi_sm8250/tree/71b13e62f057a649b77fe4062feb73ee72ad609c/drivers/input/touchscreen/fts_521) | 用于核对本机事件语义 |
| GPU / 用户空间参考 | [HackiOS XNU](https://github.com/oosexgang/hackios-xnu-kernel)、[userland](https://github.com/oosexgang/hackios-xnu-userland) | Redwood 642L 的能力不能直接归给 umi A650 |
| A650 Linux 参数参考 | [xiaomi-sm8250-devs kernel](https://github.com/xiaomi-sm8250-devs/android_kernel_xiaomi_sm8250/tree/lineage-20/drivers/gpu/msm) | 地址、资源与硬件序列须按本机核实 |
| `docs/`、当前 README 与状态档案 | leafall903 的本机研究整理 | 基于本次工作记录与机主反馈；不包含完整最新集成源码 |

本仓库尚未成为官方 HackiOS 机型支持条目，也没有被加入其核心团队名单。申请收录只应描述为社区研究链接。

## 许可

保留各来源代码中的版权、作者及许可证头。Apple 系统文件、厂商 GPU 固件和设备数据不在本次公开材料内。没有依据本仓库的维护者身份重新声明继承代码的所有权，也没有为未知许可的代码补造授权。
