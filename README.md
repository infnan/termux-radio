## USBUART
 A cross-platform library for reading/wring data via USB-UART adapters

### Introduction

`USBUART` is a LIBUSB based library that implements a relay from USB-UART 
converter’s endpoints to a pair of I/O resources, either appointed by given 
file descriptors, or created inside the library with pipe[(2)](http://linux.die.net/man/2/pipe).

User application may then use standard I/O operations for reading and writing data.
USBUART Library provides API for three languages - C++, C and Java.

See C/C++ API description in [usbuart.h](usbuart_8h.html) and Android API in 

### Usage with C++

	// Instantiate a context
	context ctx;
	
	// Attach USB via a pipe channel
	channel chnl;
	ctx.pipe(device_id{0x067b,0x0609},chnl,_115200_8N1n);
	
	//Run loop in one thread
	while(ctx.loop(10) >= -error_t::no_channel);
	
	//Read/write data in other thread(s)
	char buff[256];
	read(chnl.fd_read, buff, sizeof(buff));
	
	//or use non-blocking I/O in the loop body

## Termux + Arch proot：FT-891 / G90 的 USB 串口

`radio-bridge` 在 Termux 中通过 Android 授权的 USB 文件描述符访问串口芯片，创建 raw PTY。
Arch 中的 JTDX/Hamlib 打开 PTY，即可收发 CAT 数据。无需 root，也不要求 Android 有 `/dev/ttyUSB0`。

| USB 适配器 | 通道 | Termux 固定链接（默认目录 `~/.local/run/termux-radio/`） | Arch 固定链接 |
| --- | --- | --- | --- |
| FT-891 兼容 CP2105，`10c4:ea70`、part 5 | Enhanced，默认 interface 0 | `ft891-enhanced` | `/run/termux-radio/ft891-enhanced` |
| 同上 | Standard，默认 interface 1 | `ft891-standard` | `/run/termux-radio/ft891-standard` |
| G90 兼容单通道 FTDI，`0403:6001` / `0403:6015` | CAT | `g90` | `/run/termux-radio/g90` |

USB VID/PID、芯片型号和接口只能确认**适配器兼容性**，不能证明后面接的电台型号。普通 FT232 线也会被识别为 G90 兼容线。
默认不会发送 CAT 查询或 PTT 命令来猜测型号。CP2105 按芯片定义将 interface 0 映射到 Enhanced，1 映射到 Standard；状态 JSON 同时提供实际接口描述符。
其他 CP210x 的 `10c4:ea60/ea71` 可由库驱动使用，但不会被自动当成 FT-891。

### 在 Termux 安装和启动

先安装与 Termux 来源/签名匹配的 **Termux:API Android 应用**，然后在 Termux 中：

```bash
pkg install termux-api python clang libusb make pkg-config
make -j2
make install
termux-radio start
```

源码应放在 Termux 的私有 home 中，避免 Android 共享存储的执行权限和符号链接限制。
`make install` 使用 Termux 的 `$PREFIX`。在普通 Linux 中安装时需要自行指定有写权限的 `PREFIX`。

脚本先执行 `termux-usb -l` 检查 API 是否可用；没有命令、API 无响应、未安装配套应用时会给出安装提示。
它逐个检查列出的 `/dev/bus/usb/xxx/xxx`，优先读取可访问的 sysfs VID/PID 排除其他设备，再通过 `termux-usb -r -e` 获取权限并读 USB 描述符。
**Android 经常禁止读取 sysfs；此时在识别前就必须请求 USB 权限，可能对其他 USB 设备也出现一次授权提示。**
`termux-usb -l` 本身只返回路径，无法保证“先知道型号、再申请权限”。

服务在后台每两秒检查插拔；首次授权需解锁手机并允许提示。FT-891 建立两条 PTY，FTDI 建立一条。
拔线或桥接错误会清理失效链接并等待重连，关闭启动终端和退出 JTDX 不会主动停止服务。
Android 强制停止、系统杀进程、重启仍会中断服务；脚本无法保证进程永远不被系统回收。
服务请求 Termux wake lock，并在正常停止时释放；Android 电池设置也应允许 Termux/Termux:API 后台运行。

```bash
termux-radio status
termux-radio status --json
termux-radio stop
```

`start` 默认等待 50 秒，未就绪返回非零，但**已启动的后台服务继续等待插线**。
重复 `start` 不会启动第二套服务，也不会改变现有服务的参数。要换参数先 `stop`。
日志：`~/.local/state/termux-radio/service.log`。

### 波特率：软件同步与固定模式

PTY 自身的波特率不会影响 USB 芯片。桥接程序保留 slave FD，通过 `tcgetattr(slave)` 读取 JTDX/Hamlib 的 termios 波特率，
在发送串口数据前同步到 USB 芯片，并在事件循环中检查空闲时的变更。没有依赖 `TIOCPKT_IOCTL`，后者在 Linux 上不能作为通用串口设置通知。

| 启动入口 | 初始速率 | 后续 JTDX 改速率 |
| --- | --- | --- |
| `radio-bridge` | 默认 115200，`--baud` 优先于 `USBUART_BAUD` | 默认 `follow`，同步到 USB |
| `termux-radio start` | 默认 4800，`--baud` 优先于 `RADIO_BAUD` | 默认 `follow` |
| `--baud-mode fixed` | 使用指定初始值 | 忽略 PTY 的速率变化 |

```bash
termux-radio start --baud 4800
termux-radio stop
termux-radio start --baud 4800 --baud-mode fixed
```

环境变量也可设置：`RADIO_BAUD=4800 RADIO_BAUD_MODE=fixed termux-radio start`。
底层支持 1200/2400/4800/9600/19200/38400/57600/115200/230400；电台只接受其中的一部分。
软件同步只覆盖这些标准 termios 波特率，不支持 `BOTHER` 自定义速率；`B0` 不转发为断线。
FT-891 Enhanced 固定 8N2（与 Hamlib 的 FT-891 后端默认值一致），Standard 和 G90 固定 8N1；全部无流控。
其他数据位/校验/停止位设置不会同步，请在 JTDX 中选择对应设置。

**FT-891 的 CAT 不支持 115200。** 你家机器设为 4800 时，保持电台、脚本、JTDX 都为 4800。
FT-891 菜单 `05-06 CAT RATE = 4800`，`05-08 CAT RTS = DISABLE`。

### 在 Arch 安装和启动 WSJT-X / JTDX

Arch 与 Termux 必须运行在同一 Android UID 下，proot 必须能访问宿主的 `/dev/pts`。
proot-distro 默认配置通常已提供它；自定义 proot 启动脚本可加入：

```bash
-b /dev/pts:/dev/pts
```

不必把 Termux home 中的固定链接目录绑定进来。Arch 启动器通过 SSH 读取当前 PTY 路径，再创建自己的固定链接。
**SSH 命令不能修改已运行的 proot 的路径绑定。** 如果 `/dev/pts` 不可见，需要退出 Arch、调整启动参数后重进。

在 Arch 中安装：

```bash
pacman -S --needed python openssh zenity
install -Dm755 /你的项目路径/scripts/arch-jtdx ~/bin/arch-jtdx
~/bin/arch-jtdx
```

`zenity` 用于对话框；也支持 `kdialog`、`xmessage`，无图形环境则输出终端提示。
默认读取 `ssh u0_a299@localhost -p 8022`，使用免密码认证和已确认的 SSH 主机密钥。
Termux 端需要已经启动 `sshd`。脚本不会关闭 SSH 主机密钥检查。

启动流程：

1. 通过 SSH 获取 `termux-radio status --json`，检查对应 PTY 能否在 Arch 中以读写方式打开，且确实是 tty 字符设备。
2. 如果未连接，显示提示对话框，然后经 SSH 执行 Termux 中安装的 `termux-radio start --baud 4800`，等待并重新检测。
3. 更新 `/run/termux-radio/` 下的固定链接；FT-891 必须两条 PTY 都可访问。
4. 检测通过后默认依次查找并启动 `/usr/bin/wsjtx`、`/usr/bin/jtdx`；两者都安装时优先 WSJT-X。未安装时提示安装软件或指定自定义启动器。

用 `--launcher` 选择程序或自定义启动脚本；自定义脚本不是默认依赖。例如：

```bash
~/bin/arch-jtdx --launcher /usr/bin/jtdx
~/bin/arch-jtdx --launcher ~/bin/jtdx-fix
~/bin/arch-jtdx --launcher ~/bin/jtdx-fix -- --rig-name FT891
```

启动器参数使用 `--` 分隔并按原样传递，不经过 shell 解释。`--launcher` 只接受可执行文件路径，不接受拼接的 shell 命令。
路径支持 `~`，含空格时加引号。

可仅检测：`~/bin/arch-jtdx --check-only`（不要求安装 WSJT-X/JTDX 或自定义启动器）。只接受某台：`~/bin/arch-jtdx --radio ft891` 或 `--radio g90`。
其他用户用 `--ssh USER@HOST --port 8022`；远端命令可用 `--remote-command /绝对路径/termux-radio`。
非 root 的 Arch 用户若不能写 `/run`，用 `--link-dir "$HOME/.local/run/termux-radio"`，并在 JTDX 填对应路径。
环境变量：`RADIO_SSH`、`RADIO_ARCH_LINK_DIR`、`RADIO_BAUD`。

### JTDX 配置和通联验收

FT-891 的 CAT 串口填 `/run/termux-radio/ft891-enhanced`，G90 填 `/run/termux-radio/g90`。
选择对应 Rig，4800、Handshake None；FT-891 的 CAT 设为 8N2，G90 设为 8N1。FT-891 优先使用 **CAT PTT**。
Standard PTY 可用于数据字节收发，**不支持将 JTDX 的 RTS/DTR modem ioctl 转成物理控制线**。
Linux 普通 PTY 无法像真正的 USB 串口那样实现这些 ioctl；不能据此把 Standard PTY 当作已经完成的 RTS/DTR PTT 方案。

先 Test CAT，验证读取/设置频率，再 Test PTT 验证能进入和退出发射。
检测脚本只证明桥接进程和 PTY 可访问，不证明 CAT 协议、电台连接或发射正常。
FT-891 的非发射查询可用 `FA;`，预期收到 `FA` 加频率和分号。
G90 请使用 Hamlib 对应机型进行查询，不要套用 Yaesu 指令。
热插拔后路径保持固定，但旧进程已打开的 FD 无法自动切换到新 PTY；重新测试 CAT 或重新运行 Arch 启动器。

### 底层手工调试

```bash
mkdir -p "$HOME/.local/run/termux-radio"
termux-usb -l
termux-usb -r -e "radio-bridge --link-dir $HOME/.local/run/termux-radio --baud 4800" /dev/bus/usb/001/002
```

设备路径以实际输出为准。`radio-bridge --help` 显示全部选项；`--probe --report /绝对路径/probe.json` 只读取描述符，不创建 PTY。
CP2105 非标准映射可通过 `termux-radio start --enhanced N --standard N` 覆盖，或直接传给 `radio-bridge`。
单独手工启动时不要再运行后台服务占用同一个设备。
原来的 `ptyserial` 仍作为运行子进程的示例保留；电台连接请使用 `radio-bridge`，避免 shell 输出进入 CAT 通道。

### 实现参考与验证边界

- CP210x 的 USB 请求编号、CP2105 通道划分参考 Linux 内核 `drivers/usb/serial/cp210x.c` 和 Silicon Labs AN571；驱动在用户态通过 libusb 实现，没有加载内核模块。
- 协议说明：[Silicon Labs AN571](https://www.silabs.com/documents/public/application-notes/AN571.pdf)。
- 内核参考：[cp210x.c](https://github.com/torvalds/linux/blob/master/drivers/usb/serial/cp210x.c)、[pty.c](https://github.com/torvalds/linux/blob/master/drivers/tty/pty.c)。
- 授权接口：[termux-usb](https://github.com/termux/termux-api-package/blob/master/scripts/termux-usb.in)。
- 串口默认值：[Hamlib FT-891 后端](https://github.com/Hamlib/Hamlib/blob/master/rigs/yaesu/ft891.c)。

桌面 Linux 的编译、PTY 行为和模拟设备测试不能代替 Android 手机、电台的实机验收。


