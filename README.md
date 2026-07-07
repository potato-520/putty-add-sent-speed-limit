# PuTTY 本地增强版

本仓库基于官方 PuTTY 源码改造，在官方版本基础上进行了定制化改进，主要增加了以下能力：
1. **本地输入发送限速**：用于缓解嵌入式开发板串口输入缓冲较小、接收任务优先级较低时，复制粘贴大段文本导致接收端丢数据的问题；
2. **SSH 外部私钥自动内存导入**：让 putty/plink 直接接受 OpenSSH/ssh.com 格式的 SSH2 私钥，减少手动转换 PPK 的操作；
3. **自动日志目录配置**：支持配置自动保存日志文件的目录，方便集中管理会话日志；
4. **关闭窗口免确认**：关闭窗口时不再弹出 `Are you sure you want to close this session?` 确认对话框，直接关闭窗口。

本文件是本仓库的主 README，重点说明本地增强功能。官方原版源码说明保留在 [`README`](README)。

## 基线版本

- 官方源码版本：PuTTY Release 0.84
- 版本来源：源码中的 `version.h`
- 本次功能提交：`7243950 Add configurable send rate limit`
- SSH 外部私钥导入：putty/plink 连接时自动识别 OpenSSH PEM、OpenSSH new、ssh.com SSH2 私钥并在内存中导入
- 自动日志保存与窗口直接关闭：2026年7月定制化修改提交

- 官方原版 README：[`README`](README)

## 本次修改内容

### 1. PuTTY GUI 增加发送限速配置

在 PuTTY 图形界面中新增配置项：

```text
Terminal -> Line discipline options
Max send rate (bytes/s, 0=unlimited)
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

### 3. SSH 外部私钥自动内存导入

原版 PuTTY / plink 的 SSH2 私钥认证主要要求 `-i` 指定 PuTTY PPK 文件。本仓库新增了运行时自动识别和内存导入能力：

- 指定 `.ppk`：保持原有 PPK 加载路径。
- 指定 OpenSSH PEM 私钥，例如传统 `-----BEGIN RSA PRIVATE KEY-----`：自动在内存中导入。
- 指定 OpenSSH new 格式私钥，例如 `-----BEGIN OPENSSH PRIVATE KEY-----`：自动在内存中导入。
- 指定 ssh.com SSH2 私钥：自动在内存中导入。

示例：

```bash
plink -ssh user@example.com -i ~/.ssh/id_rsa
plink -ssh user@example.com -i ~/.ssh/id_ed25519
```

该功能不会调用外部 `puttygen`，也不会自动生成或缓存 `.ppk` 文件。导入后的私钥只保存在本次 SSH 认证流程的内存对象中。

需要注意：对于加密的 OpenSSH/ssh.com 私钥，为了在查询 Pageant 前生成公钥 blob 并保持“只尝试与配置 keyfile 匹配的 Pageant key”的行为，程序会比 PPK 路径更早提示输入私钥口令。

### 4. 自动日志目录配置 (Auto-log directory)

在 PuTTY 配置界面的 `Session -> Logging` 下，新增了 `Auto-log directory (empty to disable)` 配置项。
如果指定了目录（例如 `/var/log/putty` 或 `D:\putty_logs`），PuTTY 将自动在此目录下创建格式为 `&h_&Y-&m-&d_&t.log`（`主机名_年-月-日_时间.log`）的会话日志，采用 ASCII 格式并默认自动覆盖同名文件。若不指定则保持原有行为。
该配置对应的 session 保存键名为 `AutoLogDir`。

### 5. 关闭窗口不再弹出确认对话框

当关闭终端窗口（如点击关闭按钮、按 `Alt-F4`）时，直接销毁窗口，不再弹出 `Are you sure you want to close this session?` 确认对话框。

## 修改过的源码文件

| 文件 | 作用 |
| --- | --- |
| `conf.h` | 新增 `CONF_send_rate_limit` (限速) 和 `CONF_auto_log_dir` (自动日志目录) 配置项 |
| `config.c` | 在 GUI 界面增加限速和自动日志目录的输入框 |
| `cmdline.c` | 增加 `-sendrate` / `-send-rate` 参数解析与合法性校验 |
| `logging.c` | 在日志初始化和重配置时，自动应用 `CONF_auto_log_dir` 策略 |
| `ldisc.c` | 本地输入路径增加有序限速发送队列 |
| `windows/plink.c` | Windows plink stdin 发送路径增加限速队列 |
| `unix/plink.c` | Unix plink stdin 发送路径增加限速队列 |
| `ssh/userauth2-client.c` | SSH2 认证路径增加外部私钥自动识别与内存导入 |
| `ssh/CMakeLists.txt` | 让 SSH client 链接 `keygen` 库以复用私钥导入能力 |
| `windows/window.c` | 移除 `WM_CLOSE` 中的确认弹窗逻辑，直接关闭销毁窗口 |
| `unix/window.c` | 移除 Gtk `delete_window` 里的确认弹窗逻辑，直接关闭窗口 |
| `test/test_conf.c` | 新增 `AutoLogDir` 配置的测试用例 |

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
3. 在 `Max send rate (bytes/s, 0=unlimited)` 中输入限速值。
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

OpenSSH 私钥示例：

```bash
plink -ssh user@example.com -i ~/.ssh/id_rsa
```

如果仍指定 PPK 文件，行为保持不变：

```bash
plink -ssh user@example.com -i mykey.ppk
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
- `./build.sh plink putty pscp psftp test_conf` 构建通过。
- `plink --help` 能显示 `-sendrate bytes-per-second`。
- `plink -sendrate abc ...` 会报错退出。
- `plink -sendrate -1 ...` 会报错退出。
- 默认值 `0` 表示不限速。
- SSH 外部私钥导入相关目标已完成编译/链接验证。

## 未覆盖的验证

以下内容需要结合真实开发板现场验证：

- 不同限速值下开发板是否仍丢数据。
- 最适合目标开发板的限速值。
- 低速率下长文本粘贴的完整性。
- 特定串口驱动、USB 转串口芯片或目标系统任务调度对接收效果的影响。
- OpenSSH PEM、OpenSSH new、ssh.com SSH2 私钥的真实 SSH 服务器登录矩阵。
- 加密外部私钥错误口令重试、取消输入、服务端拒绝 key 等交互细节。
