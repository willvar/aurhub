# aurhub

AUR 宕机时的备用 RPC 镜像，数据源来自 GitHub [archlinux/aur](https://github.com/archlinux/aur)。

compile-then-serve 架构：C++23，mmap 快照 + 两级位图搜索 + `SO_REUSEPORT` epoll 服务。

## 构建

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

要求：C++23 编译器、CMake 4.0+、Ninja、zlib、CLI11、simdjson。

## 使用

```bash
# 1. 下载 AUR 镜像并构建快照（URL 自动 clone，已存在自动 fetch）
aurhub-indexer \
  --repo https://github.com/archlinux/aur.git \
  --output ./aurhub.snapshot \
  --jobs "$(nproc)"

# 2. 启动服务
aurhubd --snapshot ./aurhub.snapshot --port 9090 --workers "$(nproc)"

# 3. yay 切到本地
yay --aururl http://localhost:9090 --save
```

**定时更新**：用 cron 或 systemd timer 定时跑第 1 步，indexer 覆盖 `./aurhub.snapshot`，`aurhubd` 通过 inotify 检测变化并热重载，零停机。

## License

MIT
