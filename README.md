# WiFi Offload Manager

Linux daemon for automotive TCU — manages multi-path connectivity (WiFi + LTE) using MPTCP and cgroup-based traffic isolation.

## Documentation

- [Project Summary](docs/PROJECT_SUMMARY.md) — full design reference
- [Planning](PLANNING.md) — implementation phases and active tasks

## Quick Build

```bash
# Full image build
kas build kas/kas.yml

# Daemon only
kas shell kas/kas.yml -c "bitbake wifi-offload-manager"
```

## Target

- **Prototype:** Raspberry Pi 3B+ (wlan0 + eth0 simulating LTE)
- **Production:** TCU with wlan0 + wwan0 (LTE B2C) + wwan1 (LTE B2B)
