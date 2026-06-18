# PuTTY 发送限速改造版

本仓库基于官方 PuTTY 源码改造，增加了“本地输入发送限速”能力，用于缓解嵌入式开发板串口输入缓冲较小、接收任务优先级较低时，复制粘贴大段文本导致接收端丢数据的问题。

本文件是本仓库的主 README，重点说明本次发送限速改造。官方原版源码说明保留在 [`README`](README)。

## 基线版本

- 官方源码版本：PuTTY Release 0.84
- 版本来源：源码中的 `version.h`
- 本次功能提交：`7243950 Add configurable send rate limit`

- 官方原版 README：[`README`](README)

## 本次修改内容

### 1. PuTTY GUI 增加发送限速配置

在 PuTTY 图形界面中新增配置项：

```text
Terminal -> Line discipline options
Maximum bytes sent per second (0 for unlimited)
```

含义：

- `0`：不限速，保持 PuTTY 原始行为。
- `N > 0`：本地输入最多按约 `N bytes/sec` 的节奏发送。

该配置会随 PuTTY session 一起保存，保存键名为：

```text
SendRateLimit
```

### 2. plink 增加命令行参数

plink 新增参数：

```bash
plink -sendrate <bytes-per-second> ...
```

也支持等价写法：

```bash
plink -send-rate <bytes-per-second> ...
```

示例：

```bash
plink -serial COM3 -sercfg 115200,8,n,1,N -sendrate 100
```

表示通过 plink 发送本地 stdin 内容时，最多按约 `100 bytes/sec` 的节奏送入后端连接。

非法参数会直接报错退出，例如：

```bash
plink -sendrate abc example.com
plink -sendrate -1 example.com
```

## 修改过的源码文件

| 文件 | 作用 |
| --- | --- |
| `conf.h` | 新增 `CONF_send_rate_limit` 配置项，默认值为 `0`，保存键为 `SendRateLimit` |
| `config.c` | 在 PuTTY GUI 的 Terminal / Line discipline options 页面增加输入框 |
| `cmdline.c` | 增加 `-sendrate` / `-send-rate` 参数解析，并校验必须为非负整数 |
| `ldisc.c` | PuTTY GUI 本地输入路径增加有序限速发送队列 |
| `windows/plink.c` | Windows plink stdin 发送路径增加限速队列 |
| `unix/plink.c` | Unix plink stdin / BRK 发送路径增加限速队列 |

## 限速原理

原始 PuTTY / plink 在收到键盘输入、粘贴内容或 stdin 数据后，通常会尽快调用 `backend_send()` 送入后端连接。对嵌入式串口设备来说，这种瞬间发送可能超过开发板接收能力。

本次改造把“直接发送”改为“先进队列，再按节奏发送”：

1. 本地输入数据先进入发送队列。
2. 如果限速值为 `0`，队列会尽快排空，行为接近原版 PuTTY。
3. 如果限速值大于 `0`，每次只发送 1 个字节。
4. 发送 1 个字节后，根据配置计算下一次允许发送的时间。
5. 未到时间时，通过 PuTTY 原有 timer 机制等待下一次发送。

时间间隔计算方式：

```text
interval = ceil(1000ms / bytes_per_second)
```

例如：

| 配置值 | 发送节奏 |
| --- | --- |
| `10` | 约每 100ms 发送 1 字节 |
| `100` | 约每 10ms 发送 1 字节 |
| `1000` | 约每 1ms 发送 1 字节 |

该算法是保守策略：不会在空闲后补偿式突发发送，也不会在首次粘贴时先冲出一整秒额度。这样更适合保护小缓冲串口设备。

需要注意：PuTTY 当前 timer 精度按毫秒计算，因此当配置值高于约 `1000 bytes/sec` 时，实际速度可能低于配置值，但仍不会超过配置值。

## 有序事件队列

除了普通字节数据，PuTTY / plink 还会发送一些特殊事件，例如：

- EOF
- Unix plink 中的 BRK
- Telnet special，例如 EOL、EC、IP、SUSP

如果只限速普通字节，而让这些特殊事件立即发送，就可能出现顺序错误：前面的数据还在慢慢发送，后面的 EOF 已经先到达远端。

因此本次实现使用有序事件队列，把普通 DATA 和 SPECIAL 事件都放入同一条队列中，按产生顺序发送。这样可以避免 EOF、BRK 或 Telnet special 越过前序数据。

## 使用说明

### PuTTY GUI

1. 打开 PuTTY。
2. 进入 `Terminal -> Line discipline options`。
3. 在 `Maximum bytes sent per second (0 for unlimited)` 中输入限速值。
4. 如需保存，回到 `Session` 页面保存当前 session。
5. 连接开发板后再复制粘贴大段文本。

建议从较小值开始测试，例如：

```text
50
100
200
500
```

根据开发板实际接收能力逐步调大。

### plink

串口示例：

```bash
plink -serial COM3 -sercfg 115200,8,n,1,N -sendrate 100
```

SSH 示例：

```bash
plink -ssh user@example.com -sendrate 200
```

不限速：

```bash
plink -sendrate 0 example.com
```

## 构建说明

本仓库保留了用于 WSL 调用 Windows 工具链构建的脚本：

```bash
./build.sh
```

构建产物目录：

```text
build-vs/Release
```

本次改造已通过该脚本构建，生成：

```text
putty.exe
plink.exe
```

## 已验证内容

- `./build.sh` 构建通过。
- `plink --help` 能显示 `-sendrate bytes-per-second`。
- `plink -sendrate abc ...` 会报错退出。
- `plink -sendrate -1 ...` 会报错退出。
- 默认值 `0` 表示不限速。

## 未覆盖的验证

以下内容需要结合真实开发板现场验证：

- 不同限速值下开发板是否仍丢数据。
- 最适合目标开发板的限速值。
- 低速率下长文本粘贴的完整性。
- 特定串口驱动、USB 转串口芯片或目标系统任务调度对接收效果的影响。
