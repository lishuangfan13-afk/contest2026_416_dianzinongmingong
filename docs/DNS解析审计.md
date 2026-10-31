# BES 当前固件 DNS 独立审计

审计日期：2026-10-31。对象为 `/root/vela` 实际 NuttX DNS 源码、当前 headless 构建配置和本轮独立函数测试。独立审计阶段只读；主线随后集成了下述修复，并完成本文末尾的实板对照。

## 结论

已确认两个 DNS TCP 回退实现缺陷，并在自有目录用实际源码原函数复现、验证最小修正。**它们不能据目前证据解释这次普通 UDP DNS 请求约 15 秒失败。** 主线 PC 在同一 Pro 热点收到的小米域名回复只有 149～152 字节，TC=false，没有触发 TCP 回退或 512 字节上限。

当前 `getaddrinfo=1` / mbedTLS UNKNOWN_HOST 是多种 DNS 失败汇总后的结果，不能据此判断是“域名不存在”“DNS服务器错误”“回包解析错”还是“完全没有回包”。板上 127.0.0.1 真 resolver 应答对照，是区分解析软件和外部数据链路的有效下一项证据。

## 当前实际配置和正常链路

| 项目 | 已核对事实 | 对本次的含义 |
| --- | --- | --- |
| nameserver 存储 | `CONFIG_NETDB_RESOLVCONF=n`，内存服务器槽数 1；默认 NOADDR | 不依赖 `/etc/resolv.conf`；新加不同地址会替换旧槽，不能把多次 `--dns` 当多个备用DNS同时保留 |
| DNS 端口 | `dns_add_nameserver()` 对 sin_port=0 自动填 `HTONS(53)` | 当前 `zhaoxi --dns` 零初始化 sockaddr 是正确的，不会向端口0查询 |
| DHCP 安装 | 已审 BSP 调 `netlib_obtain_ipv4addr`，其在 DHCP DNS 非零时调用 `netlib_set_ipv4dnsaddr` | 不是单纯漏装 DHCP DNS；安装返回和当时槽中地址仍须实测 |
| 超时/重试 | recv/send 各5秒，重试3次 | 当前 AF_INET 下，三次无回应接收可得到约15秒；时间本身不证明无回包 |
| socket | 每个查询新建UDP socket；`connect()` 到指定DNS的地址/端口后 send/recv | `dns_bind` 名称虽含 bind，实际不指定本地端口；不属于“所有查询挤在固定源端口”的实现 |
| 查询地址族 | NuttX 默认 AF_UNSPEC，产品初始化已调用 `dns_set_queryfamily(AF_INET)` | 直接设置 getaddrinfo hints AF_INET 本身不抑制底层先查AAAA；当前产品的全局family设置才负责抑制AAAA |
| 缓存 | 8项，TTL计时使用 CLOCK_MONOTONIC；改DNS地址或query family会清缓存 | 板子实时时钟为1970不会直接让DNS缓存和DNS报文失效；TLS时间校验是另一层 |
| 最大回复 | UDP、TCP都受 `CONFIG_NETDB_DNSCLIENT_MAXRESPONSE=512` 限制 | TC回退存在，但TCP不会自动把缓冲增大；超过512的TCP回复仍报EMSGSIZE |

来源：当前 `.config:2554-2570`；`lib_dnsaddserver.c:311-421`；`lib_dnsbind.c:71-117`；`lib_dnsqueryfamily.c`；`lib_dnscache.c:147,228`。

## 确认缺陷 1：DNS TCP 短读/短写后指针推进错误

`lib_dnsquery.c:143,189`：

```diff
- buf = (FAR const uint8_t *)buf + len;
+ buf = (FAR const uint8_t *)buf + ret;
- buf = (FAR uint8_t *)buf + len;
+ buf = (FAR uint8_t *)buf + ret;
```

len 是本轮请求的剩余总长度，ret 才是本轮真正收发的字节数。首轮只完成3/8字节时，旧函数的第二轮从 +8 继续，正确位置应是 +3。短写会跳过原数据、可能从缓冲外发送；短读会把尾部放错位置，可能越过调用者为当前DNS记录预留的范围。

实际原函数隔离复现：send返回8，但第二次offset=8；recv返回8，但内容不等于原8字节。建议修正后offset=3，接收内容完整一致。

## 确认缺陷 2：UDP 截断转 TCP 前漏关旧 socket

`lib_dnsquery.c:903-906` 和 `962-965`：当 UDP 回复 TC=1，代码置 stream=true 后直接 `goto try_stream`，跳过当前分支末尾的 `close(sd)`，并在下一轮重写 sd。

最小修正是在 IPv6、IPv4 两个 goto 前各补 `close(sd)`。隔离复现：旧函数一次成功回退创建2个socket，只关1个；建议修正后创建2个、关闭2个、泄漏0。

已集成差异：[0024](../patches/0024-nuttx-dns-tcp-short-io-and-close.patch)。固定 Git 基线应用及实际源码比较通过；补丁交付采用原始 LF。

## 查询构造与回复解析

- 普通查询构造包含随机程度有限的单调时钟ID、RD=1、1个QNAME、IN class、A/AAAA type；多字节字段使用网络序。未发现当前普通小米域名被编码错误的证据。
- `dns_recv_response` 检查头长度、TC、RCODE、ID、问题数、原问题名、问题type/class；回答中的名字支持通过压缩指针跳过。
- CNAME记录本身被跳过，随后同一答案区的A记录可以被取出；不会对仅有CNAME、没有A的回答另发一次别名查询。
- 用实际解析函数测试：普通A成功；压缩CNAME后跟压缩名字A成功；TC请求TCP；NXDOMAIN被汇总为 `-EPROTO`。
- 解析器不是完整的安全DNS实现：问题名比较采用原编码字节精确比较，不接受被压缩/大小写改写后的等价问题；跳过压缩名字时不验证其指向的目标；读取问题/回答固定字段前缺少完整的“剩余4/10字节”检查。以上是健壮性边界，未据当前报文证明触发，不应当成此次根因。

## 错误码为什么没有区分力

1. `dns_recv_response()` 将 DNS RCODE非0统一变成 `-EPROTO`；无回包则可能是socket超时 `-EAGAIN`；其他解析错有 `-EBADMSG/-EILSEQ`。
2. `lib_gethostentbynamer.c:779-788` 在上述DNS查询最终失败后统一设置 `*h_errnop = HOST_NOT_FOUND`，其值为1。
3. `lib_getaddrinfo.c:396-402` 将这个整数原样返回。在本树 netdb.h 中 `EAI_AGAIN` 也等于1，所以打印 getaddrinfo=1 无法还原真正底层失败原因，甚至不能把它直接当作服务端NXDOMAIN。
4. mbedTLS `net_sockets.c:194-195` 把任何非零 getaddrinfo 结果统一映射为 `MBEDTLS_ERR_NET_UNKNOWN_HOST`。这时尚未完成TCP连接，不涉及API key或服务端HTTP鉴权。

建议诊断直接记录：本次 nameserver地址/端口、实际 query type、发送返回、接收返回/errno、DNS header ID/RCODE/TC。无需记录密钥或完整应用对话。

## 并发与锁

DNS nameserver/cache/notify由递归锁保护。`dns_foreach_nameserver` 在锁内复制当前服务器地址，再 breaklock 后执行可能阻塞的网络回调，返回时 restorelock；不在收包等待期间持续占用列表锁。每个 `dns_query` 单独分配 qdata，每次创建独立socket，因此当前多线程查询不会共用同一个接收缓冲。

`g_query_family` 是全局设置，setter加锁、reader直接读；产品仅在初始化时设为AF_INET，未发现本次运行中反复切换。未证明整个解析库在任意设置并发下均正确，但没有发现能解释当前现象的固定锁死路径。

## 已完成的测试和未验证内容

`tools/check_dns.py`（当前源码回归）与独立审计复现脚本 从实际文件提取原函数，只有socket收发/创建/关闭边界使用替身；使用宿主cc + ASan/UBSan。原缺陷复现3组、最小修正后3组，全部退出0且无sanitizer报告：

| 对照 | 原函数 | 最小修正 |
| --- | --- | --- |
| TCP短写第二次指针 | offset8，错误 | offset3，正确 |
| TCP短读8字节内容 | 不一致 | 完全一致 |
| TC回退socket | 创建2/关闭1 | 创建2/关闭2 |
| 普通A与压缩CNAME+A | 均成功 | 均成功 |

结果、真实源文件SHA256：`dns_repro_results.json`。这些是软件边界测试，不替代板上的 resolver、Wi-Fi、DNS或互联网验收。

主线提供的 `work/bes-runtime-20261030/dns_pc_same_hotspot.json` 记录：同一Pro热点，网关DNS收到149字节/25ms，223.5.5.5收到152字节/55ms，8.8.8.8收到149字节/204ms；均RCODE=0、TC=false、5个answer。因此当前实际小米回复可放入512字节，不能把TCP缺陷或缓冲上限直接认定为这次故障原因。

当前仍需主线板上真resolver对照来区分：正常A/CNAME回包能否在实际NuttX栈被收取并解析；若loopback成功，而外部DNS没有返回，则继续查外部UDP/路由/无线数据；若loopback失败，再根据收到的真实字节与底层errno定位解析或socket实现。

## R10 主线实板对照

完整镜像已编译并通过 AP-only 软件引导复位烧入。真实 NuttX `getaddrinfo` 通过本机 UDP fixture 验证，未使用外部网络：

| 对照 | 结果 |
| --- | --- |
| 普通 A | 实际发出查询、收到响应、返回预期文档地址 192.0.2.123 |
| CNAME 后跟 A | 实际发出查询、收到响应、返回同一预期地址 |
| NXDOMAIN | 实际收到否定响应，getaddrinfo 返回非零，符合预期 |
| 诊断结束 | 原 DNS 配置恢复，Agent 干净重启 |

这些是合成 DNS 应答，仅证明本机 UDP 与正常解析路径在实板可工作，不算外部小米域名已经解析。

当前启动后尚未取得 DHCP 地址，DNS 列表为空。手工配置 223.5.5.5 后，实板只读核对为 AF_INET、端口53；原始 UDP 诊断返回 `sent=-1 received=-1 rc=-101 elapsed_ms=0`。NuttX `101` 为 ENETUNREACH：当前查询未成功发出，失败发生在响应解析之前。前后接口 IP 与网关仍为0。**这个结果只解释当前无IP时的失败，不能倒推此前取得IP后超时的唯一原因。**

下一次取得IP时，使用以下入口记录实际DNS、原始应答和系统解析：

```text
zhaoxi --dns-info
zhaoxi --dns-probe 223.5.5.5 token-plan-cn.xiaomimimo.com
nslookup token-plan-cn.xiaomimimo.com
```

`--dns-probe` 使用独立 UDP socket，输出发送、接收、errno、耗时及响应 ID/RCODE/TC/answer数，不修改DNS或路由。`--dns-selftest` 暂时替换服务器，必须先停止Agent并等待真实退出，结束后恢复原服务器；默认不在开机时执行。

协议对照：[RFC 1035 DNS 传输与报文](https://www.rfc-editor.org/rfc/rfc1035.html#section-4.2)。现场结果：[本轮实板证据](bringup_logs/20261031_dns_board.json)。


### 真实小米回复回放

另外保存了同一热点下从223.5.5.5收到的实际149字节回复，含5个answer、TC=false；固定报文位于 `tests/headless/wifi/mimo_dns_reply_20261031.txt`。`tools/check_dns.py` 将该报文交给实际NuttX解析源码，在宿主ASan/UBSan下成功取出A地址，无TCP回退。这是实际报文的软件解析对照，未把主机替身当作板上公网查询。

随后限定两分钟观察板子自动重连，没有捕获到有效IPv4，因而本轮没有新增有效IP状态下外部DNS的验收。Agent保持运行，继续优先手机热点。
