# aurhub

[中文](README.md)

An offline RPC mirror for the AUR, backed by the [archlinux/aur](https://github.com/archlinux/aur) GitHub repository.

C++23 compile-then-serve architecture: mmap snapshot, two-level bitmap search, `SO_REUSEPORT` epoll server.

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

Requirements: C++23, CMake 4.0+, Ninja, zlib, CLI11, simdjson.

## Usage

```bash
# 1. Build the mirror and snapshot (auto-clone on first run, auto-fetch after)
aurhub-indexer \
  --repo https://github.com/archlinux/aur.git \
  --output ./aurhub.snapshot \
  --jobs "$(nproc)"

# 2. Start the server
aurhubd --snapshot ./aurhub.snapshot --port 9090 --workers "$(nproc)"

# 3. Point yay at the local mirror
yay --aururl http://localhost:9090 --save
```

For scheduled updates, use the provided systemd timer (see [`deploy/`](deploy/)). The indexer overwrites the snapshot and `aurhubd` hot-reloads via inotify with zero downtime.

## Deployment

After installing the AUR package, a single command:

```bash
systemctl enable --now aurhubd
```

The `StateDirectory` (`/var/lib/aurhub`) is created automatically. The mirror is cloned and indexed on first start, and the timer keeps it synchronized hourly.

## License

MIT
