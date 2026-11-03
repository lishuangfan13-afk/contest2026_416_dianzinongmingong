# 开机选网候选固件

此 AP 镜像与 2026-11-03 最终实板有线／无线成功测试一致。仅写 `0x30190000`，不包含个人语音模板、运行凭据或持久数据。

它保留独立候选路径，未替换此前新闻／飞书发布镜像。[验收记录](../../docs/bringup_logs/20261103_boot_uplink_acceptance.json) 列出已通过证据与尚未完成的观察，`hardware_functional_acceptance_complete` 保持 false。新板部署还需要本地选网提示音、个人模型与安全启用配置，见[使用说明](../../docs/开机语音选网.md)。
