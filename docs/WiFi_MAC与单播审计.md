# R7 MAC、DHCP DNS 与单播路径定向复核

依据：主线 R7 实板日志 `wifi_gateway_awake_r7.log`、实际 BSP 对象反汇编、当前 NuttX/apps 源码。未操作串口，未更改主线源码或固件。

## 目前结论

AP 侧没有找到 MAC 被复制错位、长度写错、普通重复 ifup 重写 radio MAC 的证据。不能由每次 boot / radio 完全关闭后 MAC 改变，就推出关联地址与数据地址不一致。该假设可以通过两处独立只读查询直接检验，值得做；当前尚未得到这组实测。

## MAC 取值链

1. `netdriver_ifup` 先检查 radio 是否已关闭。已打开时只设置接口活动位、通知 PM 并记活动时间，不重新初始化 MAC。
2. 完全关闭时调用 `bes_netdev_wifi_open`，先成功执行 `bes_wifi_initial/bwifi_init/wifi_hal_init`。
3. 随后对 devnum 0/1/2 各调用 `bwifi_get_dev_mac`，把返回缓冲的前 4 字节及随后 2 字节复制到该接口 lowerhalf 的 `+0x4a/+0x4e`。这与当前 `offsetof(lowerhalf, netdev.d_mac)=0x4a` 相符，共 6 字节。
4. `bwifi_get_dev_mac` 调 whops 表偏移 0x4c，该表的重定位实际指向 `wifi_hal_get_dev_mac`。
5. `wifi_hal_get_dev_mac` 发送 RPC command 30，请求内容是 1 字节 devnum；回复数据复制到调用者提供的缓冲。AP 侧没有另行生成一个用于 Ethernet 的随机 MAC。
6. `wifi_hal_init` 为 RPC command 11，初始化请求仅 1 字节模式；MAC 的生成、保存、实际关联地址由 APC1 内部实现，当前没有源码。

需要记下的一处底层健壮性缺陷：`bes_netdev_wifi_open` 没有检查三次 `bwifi_get_dev_mac` 的返回值，就复制栈缓冲。如果 getter 失败，可能写入无效缓存地址。当前板子的地址有稳定厂商前缀，尚无 getter 失败的直接证据，不能把这个潜在路径当成已确认根因。

## 如何做有区分度的只读对照

- 同一时刻读取 `bwifi_get_dev_mac(0, buffer)`，打印返回码和前 6 字节。
- 读取 `SIOCGIFHWADDR` 或 ifconfig 的 HWaddr。`nuttx/net/netdev/netdev_ioctl.c:1089-1096` 明确从缓存 `d_mac` 复制，属于与 radio getter 不同的来源。
- 在一次干净 ifup 后、关联/DHCP 后分别对照；不要先人为设置 MAC。
- 如用户方便，手机热点“已连接设备”页的客户端 MAC 可以作为 AP 所见关联地址的第三来源。

不能先用 `SIOCSIFHWADDR/netlib_setmacaddr` 强设随机地址：`netdev_ioctl.c:1117-1124` 仅更改缓存，当前 BSP ifup 又从 radio 查询覆盖，没有找到把该用户设置写回 radio 的路径。强设反而可能制造正在排查的两端地址不一致。

## reason 7 与省电命令

- 实板在关闭 PS 约 40ms 后，底层上报 remote reason 0007、locally_generated=0。
- 当前 NuttX IEEE80211 定义中 7 对应 CLASS3_FRAME_FROM_NONASSOC_STA；BSP event thread 原样取断线 payload，未发现 AP SDK 把 DNS/超时错误改成这个原因。
- `bes_wl_set_ps_mode` 从当前 iwreq power.flags 的正确偏移 22 取值，devnum 从设备字段取值；`wifi_hal_ps_mode_set` 经 command 38 发送控制头及 iftype/mode。没有发现该 AP 侧路径写 MAC 或重新 init radio。
- 因此不能把时间邻近直接解释为“关闭PS把MAC改错”。可能是唤醒后发数据/NULL 帧，才获得 AP 已不认当前 STA 的响应；也可能有其他底层状态问题。当前缺少空口帧及 APC1 诊断，不能确定具体原因。

## DHCP、ARP 与加密的证据边界

- 成功取得 DHCP 租约证明真实上行请求及下行响应已发生，不能再表述成“仅证明接收、完全没有证明发射”。
- DHCP 下行可能是广播，因而仍不能证明下行单播的 PTK、目标 MAC 过滤和数据接收正常。协议阶段的 4-way 成功也不能替代实际数据单播验收。
- 网关 ARP 缓存存在不必然证明有过独立 ARP 往返：`net/arp/arp_ipin.c:93-100` 在 CONFIG_NET_ARP_IPIN 下可从同网段收到的 IPv4 Ethernet 源地址学习映射；DHCP/网关广播可贡献此证据。
- 当前 RX 仅少量 DHCP UDP、无 ICMP 回包，定位仍在实际数据通信层；不是 API 密钥或模型名字导致无线数据不通。

## DHCP 的 DNS 安装确实实现

BSP `bes_netdev_connect+0xae` 调当前 apps 中 `netlib_obtain_ipv4addr(ifname)`。

`apps/netutils/netlib/netlib_obtainipv4addr.c:54-90` 依次设置 IP、mask、gateway；在 CONFIG_NETDB_DNSCLIENT 下，若租约 DNS 非零，则调用 `netlib_set_ipv4dnsaddr(&ds->dnsaddr)` 并检查结果。因此该连接路径并非漏掉 DNS 安装。

将 DNS 仅改成手机网关、保持 IP 不变，是合理对照，可排除外部 UDP53 被热点过滤这一解释。但手机网关若不提供 DNS 服务，其失败不能单独证明所有单播均坏。真正的互联网验收仍需 DNS 返回及真实 TCP/TLS/HTTP 请求。
