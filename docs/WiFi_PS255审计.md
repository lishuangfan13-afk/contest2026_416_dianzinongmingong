# R7 默认 PS=255 定向复核

结论：**在当前本地 AP/APC1 配套库中，255（0xff）是有意使用的“恢复/使用默认 PS 配置”约定，不是未初始化值，也不是一个可直接判非法的模式。**

## AP 侧来源

- `configs/apaudio3/libnx_bestbsp_ap.a` 中 `bwifi_interface.o` 的 `.data._wifi_ps_cfg` 实际字节为 `ff ff ff`，是有初值的数据节。
- `bwifi_change_current_status` 对 IDLE 硬编码调用 `bwifi_set_ps_cfg(0,1)`。
- CONNECTED 阶段硬编码 `bwifi_set_ps_cfg(0,0)`。
- GOT_IP（内部状态7）时，读取 `_wifi_ps_cfg[0]`，再调用 `bwifi_set_ps_cfg(0,255)`。
- `bwifi_get_ifa_ps_cfg/bwifi_set_ifa_ps_cfg` 访问同一数组，说明它是保存的接口 PS 偏好；WAPI power_save 直接调 `bwifi_set_ps_cfg`，不会改这个保存值。因此下一次 GOT_IP 再恢复255属于代码设计。

## APC1 接收端的明确处理

在本地 `configs/apc1/libnx_bestbsp_apc1.a` 找到并只读提取：

`wifi_hal_native.o`：`wifi_hal_ps_mode_set` 将合法接口0/1/2交给 `rwnx_cntrl_set_ps_mode`。

`rwnx_fdrv.o`：`cfgrwnx_set_ps_mode` 有以下确定分支：

- `+0x3e` 读取请求 mode；
- `+0x40` 与255比较；
- 相等时 `+0x44` 从硬件上下文取配置指针，`+0x46` 读取该配置中的默认 PS 字段；
- `+0x4a` 用默认值替换请求 mode；
- `+0x4e` 调用 `rwnx_ps_set_mode`。

因此，AP 的默认ff与APC1该实现相互契合。未经实板 APC1 镜像比对，不能证明实板恰由这份库构建；但现有证据明确反对“255本身是未初始化/非法”的推断。

## IFF_RUNNING 的边界

`nuttx/net/netdev/netdev_ioctl.c:1080-1081` 的 SIOCGIFFLAGS 直接返回 `dev->d_flags`。`getifaddrs()` 又从该 ioctl 取得 `ifa_flags`。

所以当前 netmgr 的 wlan0 + UP/RUNNING + 非零IP检查是**内核缓存的接口/carrier状态**，不是每次都向 APC1/radio 查询是否仍与手机关联。正常断线事件会清 carrier/IP，但若事件尚未到达，缓存短期滞后仍可能存在；它更不能证明 DNS、TCP 或互联网可达。

用户报告热点当前无连接设备，应与“当前时刻”的底层关联查询或手机客户端页交叉核对，不应由旧 GOT_IP 日志覆盖最新人证。

本轮未操作串口、修改配置或烧录固件。提取的闭源对象仅存独立审计目录，供本地复核。
