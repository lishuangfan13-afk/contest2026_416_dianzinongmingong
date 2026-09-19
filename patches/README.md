# Patches

These patches contain the team's contributions to public openvela repositories.
They are provided here for reference and reproducibility. Formal PRs to the
upstream repos are in progress.

## How to apply

All patches are based on the `dev-ai-contest-2026` branch. To apply them in
order:

```bash
cd packages/ai_agent
git am /path/to/contest2026_416_dianzinongmingong/patches/0001-*.patch
git am /path/to/contest2026_416_dianzinongmingong/patches/0002-*.patch
git am /path/to/contest2026_416_dianzinongmingong/patches/0005-*.patch
git am /path/to/contest2026_416_dianzinongmingong/patches/0006-*.patch

cd vendor/bes
git am /path/to/contest2026_416_dianzinongmingong/patches/0003-*.patch
git am /path/to/contest2026_416_dianzinongmingong/patches/0004-*.patch
```

## Patch list

| # | File | Repository | Description |
|---|------|-----------|-------------|
| 1 | `0001-packages_ai_agent-fix-HTTP-Date-time-sync.patch` | `packages/ai_agent` | Add HTTP Date header sync for TLS clock + build-time fallback |
| 2 | `0002-packages_ai_agent-mount-littlefs-on-data.patch` | `packages/ai_agent` | Mount littlefs on /data with tmpfs fallback |
| 3 | `0003-vendor_bes-enable-littlefs-driver-and-DNS.patch` | `vendor/bes` | Enable LittleFS driver and set default DNS server (223.5.5.5) in defconfig |
| 4 | `0004-vendor_bes-boot-into-zhaoxi_ui.patch` | `vendor/bes` | Change rcS.ap to launch zhaoxi_ui instead of lvgldemo |
| 5 | `0005-packages_ai_agent-add-daily-cron-schedule.patch` | `packages/ai_agent` | Add `daily` (fixed time-of-day) schedule kind to the cron engine and `cron_add` tool |
| 6 | `0006-packages_ai_agent-write-network-status-file.patch` | `packages/ai_agent` | Write `/data/ai_agent/NET_STATUS` on network state change for UI polling |
