# BES 无屏固件 Wi-Fi 审计

审计日期：2026-10-30。对象：R6 所用实际 SDK、NuttX 配置及预编译 BSP，另核对主线实板日志提供的事实。本文没有操作串口、重启板子或修改共享 SDK/仓库；不含密钥、Wi-Fi 密码和完整私密配置。

## 结论

不能把当前固件判定为“软件完全没有错误”。已经确认连接状态判断、TCP 建连超时和 Agent 关闭/重启生命周期有缺陷。它们能够解释掉线后恢复不足、请求长期停在进度提示，以及人工联调时出现连接竞争；尚未证明它们就是单实例新开机后 ASSOC_REJECT 的根因。

反汇编检查没有发现已经核对的 netdev lower-half、IPv4 字段和 IOB 缓冲 ABI 不匹配。Wi-Fi 事件表、WPA2 参数及 NSH 对普通引号的处理也没有发现当前 Pro 场景的写错。APC1 无线固件、802.11 原始拒绝码及 RF 实际输出仍不可从这些源码完全证明。

## 确认的软件缺陷

### 1. 关闭未完成时重启可产生两个 Agent 和两个网络连接者

R6 `packages/ai_agent/src/agent_main.c`：全局 `g_shutdown_requested`，约第 98 行；`ai_agent_main` 开始直接清 false，约第 509 行；没有单实例保护。`agent_task_create` 在 `include/agent_compat.h:91-99` 创建并 detach 线程。关闭流程仅 `message_bus_wakeup()`、各 stop 和固定 500ms 等待，随后清工具表、模型缓存和 TLS pool，未等待后台网络连接、Agent loop、出站 TTS 完全结束。

主线另外发现运行脚本把停止提示中“请等待 Shutdown complete”当成真实完成日志。这会在旧实例尚未退出时启动新实例。上述代码状态使此操作存在真实竞争，不能把这种情况下的射频/连接行为作为干净单实例基线。

最小修复建议：主入口互斥单实例；所有 worker 创建前计数、失败回滚、退出递减；关机先置 flag 和 wake bus，等待 network watcher 退出，停止录音与 cron，再等待所有相关 worker 结束，然后释放资源和实例锁。若关闭等待超时，保留 stopping 状态，不允许重新初始化共享状态。

现成接口核对：

- `agent_loop.h` 仅 init/start，无 stop/join；Agent loop 中的并行工具子线程有显式 join。
- 出站任务没有可 join 的公开句柄，会同步执行语音合成。
- 录音线程原生 pthread，`voice_channel_stop()` 会 join；ASR worker 走 `agent_task_create`。
- `cron_service_stop()` 只置 false，需在锁内 broadcast 唤醒 cond，wait 后重查 running。
- WebSocket 仍在当前 main 中无条件启动；ws_server.c 的 accept/client 两处直接创建 detached pthread，不经过 task helper。首版未使用 WS，产品配置中不启动它是最小方案；若保留，则也须纳入关闭跟踪。

### 2. HTTP/TLS 直连没有业务级 TCP 建连截止时间

R6 `src/infra/vela_tls.c:302` 调用 `mbedtls_net_connect()`；`apps/crypto/mbedtls/mbedtls/library/net_sockets.c:194-208` 实际先同步 DNS、后阻塞 connect。读超时在连接成功后 `vela_tls.c:326` 才设置。非 RPMSG 分支还没有发送超时。这能导致网络无响应时很久没有最终错误，进度提示不代表模型已回答。

建议：无有效 station link 时立即返回连接错误；TCP 使用 nonblocking connect + select + SO_ERROR，整个地址列表共用 15 秒 monotonic deadline；连上后设置发送超时。DNS 的限时仍由 NuttX 的固定重试配置控制，不宣称这个 TCP deadline 覆盖 DNS。

### 3. “已连接”仅靠任意非零 IPv4 判断

`src/infra/network_manager.c:33-64` 不看 UP/RUNNING，也不限制 wlan0。其他接口尤其临时 SoftAP wlan1 的本地地址可能抑制 station 重连。

建议产品下要求 wlan0、IFF_UP、IFF_RUNNING 和非零非回环 IPv4。`network_wait_connected()` 使用 CLOCK_MONOTONIC，避免校时改变等待长度。

已核对本 NuttX `libs/libc/net/lib_getifaddrs.c:225-251` 确实通过 SIOCGIFFLAGS 填 `ifa_flags`，所以增加这个检查有源码支持。

BSP 的正常断开回调会 carrier_off 并清 IPv4 三字段，因此不能把“旧 IP 常驻”当成本次确定根因。

### 4. DNS 默认双栈等待会增加失败等待时间

当前 IPv4/IPv6 均开启，DNS 接收/发送超时各 5 秒，重试 3 次。`libs/libc/netdb/lib_dnsquery.c:865-975` 先 AAAA 后 A，正常单次无回应最多约 30 秒接收等待，另有发送/ARP 等待。

仅改 getaddrinfo 的 hints 为 AF_INET 不足以消除 AAAA：本树 `lib_getaddrinfo.c:396` 的 gethostentbyname_r 调用不带 family，第 406 行才事后过滤。当前产品仅有 IPv4 DHCP，可在产品初始化中一次性 `dns_set_queryfamily(AF_INET)`，保留 IPv6 Kconfig，避免扰动 BSP 结构 ABI。

## 已核对而未发现当前场景错误的部分

- WAPI psk 未显式给 WPA 参数时默认 WPA_VER_2，算法 3 是 CCMP；当前 SDK 与手工命令一致。
- NSH_QUOTE=n 只禁用反斜杠转义。普通单引号和双引号仍被 nsh_argument/nsh_rmquotes 去掉，所以 SSID Pro 和普通字母数字密码不会因这个选项带上引号。
- wapi essid flags=1 执行关联；BES STA 路径会同步等连接，再进行 DHCP。0018 在已取得 IP 时跳过重复 renew 是合理的。
- 早期 ifup 失败后 watcher 退出的问题已由主线 0019 修复，R6 实板重试已观察到；该修复不等于射频关联已成功。
- `ifconfig wlan0 dns ...` 会重置地址的历史诊断误操作已停用；新的 dns_add_nameserver 专用命令不会改接口地址。
- 当前 DNS 在内存 nameserver 模式运行，缺 /etc/resolv.conf 本身不是故障证据。

## BSP 与缓冲 ABI 检查

从实际使用的 libbeschip_apaudio3.a、libnx_bestbsp_ap.a 提取对象，只读反汇编，并用真实 ARM 工具链和当前生成头文件编译独立 offsetof 探针。

| 项目 | 当前头文件 | BSP 访问 | 结果 |
| --- | --- | --- | --- |
| netdev_lowerhalf_s 大小 | 0x118 | 私有字段起于 0x118 | 对应 |
| netdev 在 lowerhalf 中偏移 | 0x18 | 字段访问前缀一致 | 对应 |
| flags / MAC | 0x40 / 0x4a | rxf、MAC 初始化同偏移 | 对应 |
| IP / gateway / mask | 0x50 / 0x54 / 0x58 | reset_ip / disconnect 清这些位置 | 对应 |
| iob_s 大小 | 24 | 内嵌数据位于基址+24 | 对应 |
| io_len / io_offset / io_bufsize | 4 / 6 / 8 | TX/RX 直接访问相同位置 | 对应 |
| io_pktlen / io_free / io_data | 12 / 16 / 20 | TX/RX 直接访问相同位置 | 对应 |
| NET_LL_GUARDSIZE | 72 | TX 断言要求 72 | 对应 |

检查的对象没有 DWARF 类型信息。以上排除已列字段的布局漂移，不能外推成整个闭源 BSP/APC1 协议都正确。

## ASSOC_REJECT 能证明什么

`bwifi_interface.o` 的 `wifi_event_thread+0x2d4..0x2f0` 从 event1 payload 取 state 字节，存入 `_wifi_connecting_state` 并原样调用用户 callback。`bes_netdev_ax_driver.o` 的 `on_wifi_connect` 查表打印状态。观察到的 6 对应头文件 WL_CONN_STATE_ASSOC_REJECT；AUTH_TIMEOUT=4、ASSOC_TIMEOUT=7 是不同枚举。

因此，它不是上层 Agent、DNS 或通用 connect 返回 -22 后随意起的名字。可是原始 802.11 Association Response 的 status_code 没有被当前日志打印，APC1 内部如何决定 state6 无源码可审，不能据此断言 AP 实际发送了某一拒绝码。

`bes_wl_iw_priv_command` 的 GET_CONN_STATUS 只能返回上述汇总状态、扫描数、活动时间；不能补出原始 802.11 拒绝码。后续应使用供应商 LMAC 诊断日志、已知兼容的 APC1/BSP 套件或空口抓包获取该层证据。

## 临时 SoftAP 诊断边界

- BSP MASTER(mode=3) 只允许 devnum=1，即 wlan1；wlan0 MASTER 被拒绝是实现限制，不能当发射坏。
- 支持 set_softap_config → softap_start → carrier_on → DHCP server。
- AP 侧配置成功后只复制到 `.bss._last_softap_cfg`，start/stop/config 经 RPC 39/41/42 下发。此调用链未见 nvrecord/flash 写入；APC1 handler 没有源码。
- SoftAP 广播被 PC 实际扫到，可增加 TX 方向证据。没有出现在 Windows 缓存列表中，若未确认执行真实主动扫描，则不能证明无广播或 TX 损坏。
- 诊断应先让单实例 Agent 真实停完，再启用 wlan1；结束后停止 AP 并恢复 station，避免与重连线程竞争。

## 其他边界缺陷，当前 Pro 输入不触发

- shell_quote 使用 POSIX 的嵌入单引号转义拼接，而当前 NSH_QUOTE/ARGCAT 关闭，对带单引号的 SSID/密码不应宣称任意字符串均支持。当前 Pro 及简单密码不受此问题影响。
- 切换到开放热点时仅清空保存的密码、跳过 wapi psk，未明确清除驱动内的旧加密状态。当前两热点都是加密网络，不是本次原因。
- TLS 当前为 VERIFY_OPTIONAL，没有严格 CA 校验。这是独立的现有配置事实，不会导致无线关联拒绝；应在正式交付定义里单独记录，不能拿来解释当前 Wi-Fi。

## 建议修复及实际检查

建议 diff：`proposed-network-readiness-and-tcp-timeout.patch`。只改产品分支和连接超时，不改 BSP 二进制。

已完成：

1. 两个独立 host fixture 运行 ASan/UBSan：接口有效性、SoftAP/旧 IP 误判、getifaddrs 失败；TCP 即时成功、SO_ERROR 拒绝、select 超时、EINTR 剩余预算、DNS 失败和描述符关闭，均 PASS。
2. 复用真实 compile_commands 对建议的 network_manager.c、vela_tls.c、agent_main.c 编译 ARM 对象，全部 exit 0。没有链接或烧录建议固件。
3. 独立 offsetof 探针对照上述 BSP ABI。

运行入口：`test_network_readiness.py`、`compile_proposal.py`。测试替身只验证软件边界，不算真实 Wi-Fi、DNS、TCP 或云端验收。实际修复集成、整机编译和板上验收由主线完成。
