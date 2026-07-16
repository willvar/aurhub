# aurhub

AUR 宕机时的备用 RPC 镜像，数据源来自 GitHub [archlinux/aur](https://github.com/archlinux/aur)。

纯 Go，零 CGO，零运行时依赖。

## 安装

```bash
go install github.com/willvar/aurhub@latest
```

或者克隆后本地编译：

```bash
git clone https://github.com/willvar/aurhub.git
cd aurhub
go build -o aurhub .
```

## 用法

```bash
aurhub serve -a :9090    # 启动（首次自动克隆+建索引，之后每30分钟增量同步）
aurhub status            # 查看运行状态
aurhub stop              # 停止
aurhub restart           # 重启
```

启动后 yay 切到本地：

```bash
yay --aururl http://localhost:9090 --save
```

切回官方：

```bash
yay --aururl https://aur.archlinux.org --save
```

## 架构

```
archlinux/aur (GitHub)         内存索引                HTTP RPC
┌─────────────────┐   go-srcinfo  ┌───────────┐  JSON   ┌──────────┐
│  158k 个分支     │ ────────────▶ │ 170k 条目  │ ◀───── │  /rpc    │
│ .SRCINFO        │              │ 首字母索引  │        │ /packages│
└─────────────────┘              └───────────┘        └──────────┘
      ▲                              │                      │
  git fetch                      gob 序列化           yay --aururl
  增量同步                       aurhub.gob           localhost:9090
```

- **数据源**：`git clone --bare` GitHub 镜像，增量 `git fetch`
- **解析**：[go-srcinfo](https://github.com/Jguer/go-srcinfo)（yay 作者维护）
- **索引**：内存 `[]dbEntry` + `map[string]int` + 首字母预过滤数组
- **持久化**：`encoding/gob` 二进制，76MB
- **搜索**：首字母预过滤 → `strings.Contains` 线性扫描
- **RPC**：`net/http`，v5 协议，`/rpc` + `/packages.gz` + `/health`

## 性能

| 指标 | 数据 |
|---|---|
| 全量 sync | 35s |
| 增量 sync | 6s |
| 顺序搜索 | 261 QPS |
| 顺序 info | 254 QPS |
| 并发 p99 | <2ms |
| 磁盘占用 | 76MB |
| 内存占用 | ~200MB |

## 局限

- 无维护者/投票/热度等元数据（GitHub 镜像不含）
- 无 RPC v6（yay 未适配）
- 搜索结果上限 500 条

## License

MIT
