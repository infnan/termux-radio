# termux-radio

注意：本项目面向**希望在 Android 上运行电脑版 WSJT-X 或 JTDX，已经准备好 Linux 运行环境，却卡在 USB 串口连接这一步**的用户，帮助这些软件连接电台。如果只是希望在 Android 上完成 FT8 通联，可以直接使用 [FT8CN](https://github.com/N0BOY/FT8CN)，不必为了通联搭建电脑版软件环境。

把 Android 手机连接的 USB 串口变成 Linux 软件可以打开的串口，用手机上的 **WSJT-X / JTDX 控制电台，完成 FT8 通联**。也可以给串口终端、对讲机写频工具或单片机程序使用。

无需 root。USB 设备由 Termux 宿主获取 Android 授权，后台服务建立串口；运行在 proot 发行版中的软件通过固定路径使用它。发行版可以是 Debian、Ubuntu、Arch 等，也可以由 proot-distro 管理。

本项目基于 [thingsapart/usbuart-termux](https://github.com/thingsapart/usbuart-termux) 修改，其 USBUART 库源自 [Eugene Hutorny 的 usbuart](https://github.com/hutorny/usbuart)。感谢原作者和上游贡献者；CP210x 支持参考 Linux 内核驱动，详细来源见文末。

本项目完全使用AI修改。

## 准备环境

- Android 手机支持 USB OTG，USB 线已接好电台或串口设备。
- 已安装 Termux，以及与 Termux 来源、签名匹配的 **Termux:API Android 应用**。
- 已有可以运行图形软件的 proot Linux 环境，并安装 WSJT-X 或 JTDX。
- 通联前准备好音频输入输出和系统时间。本项目负责 USB 串口连接。

如果还没有配置声音，可参考文末的“声音配置参考”附录。

项目包含 CP210x、FTDI、CH34x 和 PL2303 串口驱动。下面的流程优先以 FT-891 为例，串口参数请与自己的电台一致。

## 1. 在 Termux 宿主安装

以下命令在 **Termux 中执行，不是在发行版中**。在 Termux 的 home 下下载源码并安装：

```bash
pkg install git termux-api python clang libusb make pkg-config
cd ~
git clone https://github.com/infnan/termux-radio.git
cd termux-radio
make -j2
make install
```

插好 USB 线，解锁手机，启动服务并允许 Android 弹出的 USB 访问请求：

```bash
termux-usb-serial start
termux-usb-serial status
```

初始波特率为 **4800**；软件打开串口后设置的波特率会同步到 USB 串口。更改初始值或固定波特率：

```bash
termux-usb-serial stop
termux-usb-serial start --baud 9600
# 或固定使用 4800，不跟随软件修改
termux-usb-serial stop
termux-usb-serial start --baud 4800 --baud-mode fixed
```

服务在后台持续运行，拔线后会等待重新连接。关闭 WSJT-X/JTDX 不会停止它。手工停止：

```bash
termux-usb-serial stop
```

如果启动时还没插设备，命令等待一段时间后返回，后台服务仍继续等待。

## 2. 确认串口名称

`termux-usb-serial status` 中的 `ports` 会列出串口名和当前 `/dev/pts/...` 路径。Termux 侧的固定链接位于：

```text
~/.local/run/termux-radio/
```

USB 产品名称明确包含型号时，使用型号命名。可识别的名称包括 FT-891、IC-7300、G90/G90S/G90N、X6100、X6200、IC-705、FTDX10 和 FT-710。这里列的是**名称识别规则**；实际连接还需要设备使用项目支持的 USB 串口接口。

| 情况 | 串口名称示例 |
| --- | --- |
| 名称中有 FT-891，且为双通道 CP2105 | `ft891-enhanced`、`ft891-standard` |
| 名称中有 G90，且为单通道 | `g90` |
| 名称中有 IC-7300，且为单通道 | `ic7300` |
| 没有明确型号的 CP210x、FT232 等串口 | `ttyUSB0`、`ttyUSB1`…… |

普通 FT232 写频线会得到通用名称，不会自动当成 G90。多个设备可以同时使用；同型号设备会增加编号。

如果电台只报告芯片名称，可在确认接线后手工指定型号。例如自己的 FT-891：

```bash
termux-usb-serial stop
termux-usb-serial start --radio ft891 --baud 4800
```

这会生成 Enhanced 和 Standard 两个名称，并为 FT-891 Enhanced 设置 8N2。G90 可用 `--radio g90`。手工指定型号时，请只连接需要使用的那个串口设备。

## 3. 在发行版中使用

### 方式 A：先在 Termux 启动，再直接打开软件

这是最简单的用法，**不需要 SSH**。

Termux 中启动服务后，将固定串口目录提供给发行版。以 proot-distro 的 Debian 为例，以下命令仍在 **Termux 中执行**：

```bash
proot-distro login debian --bind "$HOME/.local/run/termux-radio:/run/termux-serial"
```

换用其他发行版时，把 `debian` 换成对应名称。自行编写 proot 启动命令的用户，将上述宿主目录绑定到 `/run/termux-serial`，并保证宿主 `/dev/pts` 在发行版中可见。

进入发行版后检查：

```bash
ls -l /run/termux-serial/
```

随后在自己的图形桌面中启动 `/usr/bin/wsjtx` 或 `/usr/bin/jtdx`，按下一节填写串口路径。

### 方式 B：发行版内一键检查、连接并启动软件

此方式通过 SSH 调用 Termux，适合希望点击启动器就自动检查 USB 的用户。已有 Termux 免密 SSH 配置可以直接沿用；初次设置见文末附录。

将项目的发行版脚本复制到发行版可访问的位置。在 **发行版内、源码目录中**安装：

```bash
install -Dm755 scripts/distro-wsjt ~/.local/bin/distro-wsjt
install -Dm755 scripts/distro-usb-serial ~/.local/bin/distro-usb-serial
```

发行版需要 Python 3、OpenSSH 客户端；图形对话框可安装 Zenity。例如：

```bash
# Debian / Ubuntu
sudo apt install python3 openssh-client zenity
# Arch
sudo pacman -S --needed python openssh zenity
```

如果当前已是 root，省略 `sudo`。安装完成后运行：

```bash
~/.local/bin/distro-wsjt
```

脚本优先使用 `/usr/bin/wsjtx`，其次使用 `/usr/bin/jtdx`。自定义启动器作为可选参数：

```bash
~/.local/bin/distro-wsjt --launcher "$HOME/bin/jtdx-fix"
```

没有可访问的串口时，提供三个选项：

- **重试**：重新检测串口；必要时调用 Termux 获取 USB 权限、启动桥接，再次检测。
- **中止**：结束，不启动软件。
- **忽略**：直接启动软件。

连接成功后，发行版固定串口目录为 `~/.local/run/termux-serial/`。需要沿用方式 A 的路径时，添加 `--link-dir /run/termux-serial`。

多个设备同时连接时，可以选择串口，例如 `--device ttyUSB0`。只连接串口、供其他程序使用：

```bash
~/.local/bin/distro-usb-serial --device ttyUSB0
```

`--check-only` 只检查并建立发行版链接，不弹框、不申请权限、不启动软件。旧入口 `termux-radio` 和 `arch-jtdx` 保留兼容；新部署请使用明确区分宿主、发行版的名称。

## 4. WSJT-X / JTDX 设置

在软件的 **Settings → Radio** 中：

1. **Rig**：选择实际电台型号。
2. **Serial Port**：填写固定串口路径。方式 A 的 FT-891 为 `/run/termux-serial/ft891-enhanced`；方式 B 默认为 `/home/你的用户名/.local/run/termux-serial/ft891-enhanced`，使用 root 时是 `/root/.local/run/termux-serial/ft891-enhanced`。按实际 home 填写完整路径，不要填写 `~`。
3. **Baud Rate**：与电台 CAT 设置一致，例如 4800。
4. FT-891 使用 **8 数据位、无校验、2 停止位、Handshake None**。其他设备按自己的 CAT 要求设置。
5. **PTT Method** 选择 **CAT**。
6. 点击 **Test CAT**，成功后测试 PTT，再配置好音频设备，开始通联。

如果使用通用名称，将串口路径最后一段换成实际的 `ttyUSB0`。FT-891 的 CAT 使用 Enhanced 通道，Standard 通道同时保留给其他用途。

## 常见问题

### 找不到 USB 设备，或一直等授权

在 Termux 执行 `termux-usb -l`，检查 USB 线、OTG 连接和 Termux:API。手机保持解锁，允许 USB 弹窗。如果没有安装配套 Android 应用，仅安装 `termux-api` 命令行包还不够；启动脚本会给出提示。

查看日志：

```bash
tail -n 80 ~/.local/state/termux-radio/service.log
```

### 显示 ttyUSB0，而不是电台型号

这通常是设备只提供了串口芯片名称。直接把 `ttyUSB0` 填入软件即可；需要型号名称或 FT-891 的双通道配置时，按前文用 `--radio` 手工指定。

### Termux 已就绪，发行版打不开串口

检查两边是否属于同一个 Termux 安装，并确认发行版能够访问链接指向的 `/dev/pts/数字`。自行配置 proot 时可绑定 `-b /dev/pts:/dev/pts`，修改绑定后重新进入发行版。固定目录绑定也要在进入发行版前完成。

### Test CAT 失败

检查是否选中了正确的电台、CAT 通道和波特率，并关闭其他占用串口的软件。FT-891 使用 Enhanced 通道和 8N2；如果设备名称不含型号，先用 `--radio ft891` 重新启动服务。PTT 使用 CAT，不选串口 RTS/DTR。

### 拔插后软件不再响应

等待宿主服务重新建立串口，再在软件中关闭、重新打开串口，或重启 WSJT-X/JTDX。有 USB 序列号的通用串口会记住编号；没有序列号的设备换 USB 地址后，可在 `status` 中查看新名称。

### 修改启动参数没有生效

已经运行的宿主服务保留原参数。执行 `termux-usb-serial stop`，再用新参数 `start`。

## 附录1：声音配置参考

**以下仅为参考配置。** 这是一种让发行版内的 WSJT-X / JTDX 使用 Termux PulseAudio 服务的配置方式，请结合自己的 Android 音频连接和发行版环境调整。

### 1. 在 Termux 宿主启动 PulseAudio

以下命令在 **Termux 宿主**执行。如果尚未安装 PulseAudio，先安装：

```bash
# 安装
pkg install pulseaudio
# 启动
pulseaudio --start --exit-idle-time=-1 --use-pid-file=no
pactl load-module module-sles-source
```

使用软件期间保持宿主 PulseAudio 服务运行。

### 2. 在发行版内连接宿主音频服务

以下命令在 **发行版内**执行，Debian、Ubuntu、Arch 等均按这一流程操作。先查找宿主的 PulseAudio socket：

```bash
export PULSE_SOCKET="$(find /data/data/com.termux/files/usr/tmp -maxdepth 2 \
    -type s -name native -print -quit 2>/dev/null)"

if [ -n "$PULSE_SOCKET" ]; then
    export PULSE_SERVER="unix:$PULSE_SOCKET"
    printf '使用 PulseAudio 服务：%s\n' "$PULSE_SERVER"
else
    printf '%s\n' '未找到宿主 PulseAudio socket，请先检查宿主服务及路径是否可见。' >&2
fi
```

看到 `使用 PulseAudio 服务：unix:...` 后，再继续后面的步骤。如果未找到 socket，先确认 Termux 中已启动 PulseAudio，并且发行版可以访问 `/data/data/com.termux/files/usr/tmp`；自行配置 proot 的用户可将宿主该目录绑定到发行版内的相同路径。

### 3. 配置发行版的 PulseAudio 和 ALSA 客户端

仍在 **发行版内**执行。下面的命令仅在文件不存在时创建配置，**不会覆盖已有配置**：

```bash
if [ ! -f "$HOME/.config/pulse/client.conf" ]; then
    mkdir -p "$HOME/.config/pulse"
    cat > "$HOME/.config/pulse/client.conf" <<'EOF'
enable-shm = no
enable-memfd = no
EOF
fi

if [ ! -f "$HOME/.asoundrc" ]; then
    cat > "$HOME/.asoundrc" <<'EOF'
pcm.!default {
    type pulse
}

ctl.!default {
    type pulse
}
EOF
fi
```

如果这些文件已经存在，请先备份，再按需要将上述配置合并进去。发行版需已安装相应的 PulseAudio 客户端组件和 ALSA PulseAudio 插件。

### 4. 在 WSJT-X / JTDX 中选择声音设备

在完成第 2 步的**同一个发行版终端**中启动软件，例如：

```bash
/usr/bin/wsjtx
# 或
/usr/bin/jtdx
```

也可从该终端运行前文的 `distro-wsjt` 或自定义启动器。打开 **Settings → Audio**：

- **Input**：`OpenSL_ES_source`
- **Output**：`OpenSL_ES_sink`

**注意：Android 不要处于静音模式，并检查媒体音量。** 电台与手机之间的音频连接也应按自己的设备接好。

重新打开终端，或宿主 PulseAudio 重启后，重新执行第 2 步，再启动软件。如果从桌面图标启动，也要让启动器获得相同的 `PULSE_SERVER` 环境变量。

## 附录2：配置发行版调用 Termux 的 SSH

**已有免密连接就跳过本节。** `distro-wsjt` 会从当前 Android UID 自动推导 Termux 用户名，默认连接 `localhost:8022`；也可用 `--ssh` 或 `RADIO_SSH` 显式指定。账号不是固定的，每台设备以 Termux 中 `whoami` 的输出为准。

在 **Termux 宿主**执行：

```bash
pkg install openssh
whoami
passwd
sshd
```

在 **发行版内**生成密钥并复制到 Termux，把 `你的Termux用户名` 替换为上一步结果：

```bash
ssh-keygen -t ed25519
ssh-copy-id -p 8022 你的Termux用户名@localhost
ssh -o BatchMode=yes -p 8022 你的Termux用户名@localhost true
```

已有密钥时复用即可，不覆盖原密钥。最后一条命令不询问密码、成功退出，就可以使用一键启动器。首次连接时核对并接受宿主密钥。

自动推导不可用，或使用了其他 SSH 主机别名时，可在发行版的 shell 配置中设置：

```bash
export RADIO_SSH=你的Termux用户名@localhost
```

自定义 SSH 端口使用 `--port`，例如 `distro-wsjt --port 8022`。每次需要一键连接时，确保 Termux 的 `sshd` 正在运行。

---

## 来源与致谢

- **thingsapart**：[usbuart-termux](https://github.com/thingsapart/usbuart-termux)，本项目直接基于的 Termux 适配版本。
- **Eugene Hutorny 及 USBUART 贡献者**：[hutorny/usbuart](https://github.com/hutorny/usbuart)，提供 USB 串口库及原有 C/C++/Java 接口。
- **Linux 内核 CP210x 驱动作者与维护者**：[drivers/usb/serial/cp210x.c](https://github.com/torvalds/linux/blob/master/drivers/usb/serial/cp210x.c)，包括 Craig Shelley、Johan Hovold 等贡献者。本项目的 CP210x 用户态驱动参考其控制请求和串口配置实现，适配为 libusb 用户态访问。

保留上游源码中的版权和许可声明，以及仓库原有的 `LICENSE`。第三方来源与许可说明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。感谢以上项目让 Android 上的 USB 串口与电台应用成为可能。
