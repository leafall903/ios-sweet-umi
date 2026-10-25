# 申请 HackiOS 官网收录

核查时间：2026-10-25。目标是请求收录 **leafall903 的 Xiaomi Mi 10 / umi 社区适配研究链接**，不是宣称加入 HackiOS 核心团队或成为正式支持机型。

## 找到的渠道

- [HackiOS 官网](https://hackios-web.vercel.app/en) 的 FAQ 将合作联系指向 Telegram [@netusd](https://t.me/netusd)；项目动态频道是 [@oosexgangdevs](https://t.me/oosexgangdevs)。该联系人是否受理社区机型收录，需要由对方确认。
- [官网仓库](https://github.com/oosexgang/hackios-web) 的 README 指出团队名单、链接等位于 `lib/site.ts`，页面内容和四语言文本位于相应页面与字典。当前未找到公开的自动登记表、社区机型目录或保证收录的规则。
- 因此，较直接的路线是先向公开联系人询问是否愿意添加社区研究链接。网站收录由对方维护者决定，不会因为创建自己的仓库而自动发生。

本次只准备介绍材料，未发送 Telegram 消息、创建上游 Issue 或 PR，也未提出代码合并。

## 当前材料与不足

已有：维护者身份、公开仓库、设备与版本、已完成/未完成状态表、多核与输入进展、性能测量边界、A650 诊断缺口、来源说明。

建议补齐：一段不剪断启动/操作过程的实机视频、去掉序列号和个人内容的启动日志、与公开提交对应的完整增量源码和构建说明。目前缺少这些公开复现材料，是申请的实际弱点；不能用其他机型视频或上游成果代替本机证明。

## 可发送的英文介绍草稿

> Hi HackiOS team, I'm leafall903. I maintain an independent Xiaomi Mi 10 (umi / Snapdragon 865 / Adreno 650) iOS 16 research project:
> https://github.com/leafall903/ios-sweet-umi
>
> On my device, an iPhone X iOS 16.7.16 research build has reached the lock screen and SpringBoard UI. Eight CPU cores have passed initialization/SGI tests, and touch and hardware keys have worked in device tests. The UI still uses a CPU framebuffer; sustained 60 fps, Adreno 650 acceleration, wallpapers, widgets, and correct blur/transparency are not solved. A650 work is currently limited to register/power-domain diagnostics.
>
> The repository documents the tested results, limitations, and upstream provenance. It currently includes inherited base patches and research notes, rather than a complete reproducible release of the latest integrated build.
>
> Would you consider linking this as an independent community research project, or tell me what evidence and source publication you require for a community device entry? I'm asking for a project link, with clear experimental status, without requesting a code merge or an official supported-device claim.
>
> Maintainer: https://github.com/leafall903
> Status and evidence: https://github.com/leafall903/ios-sweet-umi/blob/leafall903/umi-research-2026-10-25/README.en.md

如获同意，建议条目写成：**Xiaomi Mi 10 (umi) — independent community research by leafall903; iOS UI boots with CPU rendering; A650 acceleration in progress.** 对方最终采用的措辞需由其审核。
