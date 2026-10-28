# PVE 与 Linux 服务端

服务端改动有三部分：PVE 增加 SPICE streaming/control 端口、Linux 客户机运行修改后的 streaming-agent、NVIDIA Xorg 桌面运行显示器控制服务。**没有替换 PVE 的 QEMU 或 libspice-server 二进制。**

以下部署假设已有 Ubuntu GNOME/Xorg、NVIDIA 驱动与 GPU 直通、Docker，以及工作的 SPICE 连接。所有 Linux 命令应在相应服务器的仓库目录执行。首次配置显示服务会影响桌面会话，先保存应用内容。

## 1. PVE 的两个端口

在 PVE Shell 查看目标 VM 的 `qm config <VMID>` 和 `qm showcmd <VMID> --pretty`。将 [qemu-args.example](../server/pve/qemu-args.example) 中四个参数组追加到现有 `args`，不要覆盖已有 SPICE 监听地址或其他设备参数。确认已有 virtio serial 控制器 `spice.0`，且端口号 4、5 未被占用；冲突时选择未使用的编号。

配置进入运行中 QEMU 需要设备热添加或 VM 完整关机再启动。本项目的通用部署说明采用保存配置后正常关机/启动，不附带针对某台 VM 的自动热添加脚本。

客户机应出现：

```text
/dev/virtio-ports/org.spice-space.stream.0
/dev/virtio-ports/org.spice-space.displays.0
```

前者传输视频，后者传输显示器配置。已有 `spice.0`、spice-vdagent、WebDAV、USB 重定向设备需要保留。USB 重定向还需要 PVE 配置 `host=spice` 的 USB 设备与 USB3 控制器。WebDAV 需要 `spice-webdavd` 和对应端口。

固定端口连接沿用自己的 SPICE 配置。`disable-ticketing=on` 表示无密码认证，只应放在受信任网络或隧道内，不能直接作为公网访问配置。该项目不管理 SPICE TLS 或访问控制。

## 2. 构建整屏 H.264 服务

在 Linux 构建服务器的仓库根目录执行：

```bash
python3 tools/fetch-sources.py --component spice-streaming-agent-0.3 --download-only
docker build -f server/streaming/spice-fullscreen.Dockerfile -t local/spice-fullscreen-h264:0.3-dx2 .
```

下载工具需要 Python 3.12+。较旧 Ubuntu 可在构建机准备并校验发行包，再将其放入 `.build/downloads/`；服务本身使用 Ubuntu 自带 Python/GI，不需要升级系统 Python。

Dockerfile 通过 SHA256 已校验的上游 0.3 发行包应用 [服务端补丁](../server/patches/spice-streaming-agent-fullscreen.patch)，使用 Debian Bookworm 构建。公开版 Dockerfile 将开发时的本地基础镜像替换为公开基础镜像并显式列出依赖；实际运行验证使用的是相同源代码、补丁和编码参数，公开基础镜像从零重建的验证状态见 [VALIDATION.md](VALIDATION.md)。

修改内容：X11 根窗口整屏采集、画面大小变化后的管线更新、H.264 / I420 格式约束、流与显示布局处理。编码参数保留 60fps、20Mbps、`zerolatency`、`ultrafast`、关键帧间隔 120。多个屏幕共享一张扩展桌面画面；编码仍在 CPU 执行。

## 3. 配置视频服务

检查桌面用户的 UID/GID、`video` 与 `render` 组，以及 Xorg 授权文件：

```bash
id YOUR_DESKTOP_USER
getent group video render
```

将 [spice-fullscreen.default.example](../server/streaming/spice-fullscreen.default.example) 复制为 `/etc/default/spice-fullscreen`，按实际值修改。模板假设桌面为 `:0`、UID 1000、GDM 授权文件 `/run/user/1000/gdm/Xauthority`。

在仓库根目录安装：

```bash
sudo install -m 755 server/streaming/spice-fullscreen-start.sh /usr/local/libexec/spice-fullscreen-start.sh
sudo install -m 644 server/streaming/spice-fullscreen-h264.service /etc/systemd/system/
sudo install -m 644 server/streaming/99-spice-fullscreen.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=virtio-ports
sudo systemctl daemon-reload
sudo systemctl enable --now spice-fullscreen-h264
```

容器以桌面 UID/GID 运行，禁用网络、只读根文件系统、移除 capabilities；仅挂载 X11 socket、授权文件、视频端口和 `/dev/dri`。wrapper 等待 X11 与端口就绪。无客户端连接时停止画面编码。

## 4. NVIDIA 桌面与虚拟显示器

这是与硬件有关的配置。先备份 `/etc/X11/xorg.conf.d/`、`/etc/gdm3/custom.conf` 和桌面用户的 `~/.config/monitors.xml`。

1. 将 GDM 配置为 `WaylandEnable=false`，确保目标用户登录 X11 会话。
2. 阅读并修改 [90-nvidia-spice.conf](../server/nvidia/90-nvidia-spice.conf)：`BusID` 必须匹配客户机 NVIDIA 设备，`DFP-*` 必须对应独立物理连接器。示例的 DFP-0/1/3/5 对应 HDMI-0、DP-0、DP-2、DP-4；不同显卡不能直接照搬。
3. 从一个有效显示输出取得 128 字节以上 EDID，保存为 `/etc/X11/spice-virtual.edid`，然后在仓库根目录运行 `sudo python3 server/nvidia/create-spice-edids.py`，生成四个独立型号/序列号的虚拟 EDID。
4. 将核对后的配置安装为 `/etc/X11/xorg.conf.d/90-nvidia-spice.conf`。
5. 保存桌面应用后重启 GDM，使 NVIDIA 读取自定义模式选项。重启 GDM 会关闭当前桌面会话。

示例 `ModeValidation` 放宽了合成虚拟输出的非 EDID 模式限制，以便动态创建自定义尺寸；不要未经核对套用到实体显示器。`glxinfo -B` 应在目标用户 X11 会话中报告 NVIDIA 渲染器。

## 5. 多屏控制服务

客户机需要 `python3-gi`、Gio、`xrandr`（Ubuntu 包 `x11-xserver-utils`）、`cvt`，以及目标用户的 GNOME Mutter session bus。

修改 [spice-displays.service.example](../server/nvidia/spice-displays.service.example) 的 `User=YOUR_DESKTOP_USER` 和 DBus UID。在 [spice-displays.py](../server/nvidia/spice-displays.py) 中核对 `ENV` 的 DISPLAY/XAUTHORITY 和 `OUTPUTS` 的四个连接器名称；这些值与 Xorg 配置保持一致。然后安装：

```bash
sudo install -m 755 server/nvidia/spice-displays.py /usr/local/libexec/spice-displays.py
sudo install -m 644 server/nvidia/spice-displays.service.example /etc/systemd/system/spice-displays.service
sudo install -m 644 server/nvidia/99-spice-displays.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=virtio-ports
sudo systemctl daemon-reload
sudo systemctl enable --now spice-displays
```

服务以普通桌面用户运行，systemd 创建并授权 `/var/lib/spice-displays`；端口由 `video` 组以 0660 访问。首次或旧版状态默认一块 1600×900 自动屏，协议 v2 保存的布局会恢复。自定义 XRandR 模式在用完后清理，失败操作尝试恢复前一个有效布局并向客户端返回错误。

不要修改已经运行的生产配置来做补丁应用验证；在独立源码目录验证 patch。

## 6. 排障与回退

```bash
systemctl status spice-fullscreen-h264 spice-displays
journalctl -u spice-fullscreen-h264 -u spice-displays -n 100 --no-pager
docker logs --tail 100 spice-fullscreen-h264
```

在桌面用户会话检查 `xrandr --query`；从 SSH 检查时使用该会话的 DISPLAY、XAUTHORITY 和 DBUS_SESSION_BUS_ADDRESS。

- 控制服务未连接：核对专用端口、用户/组、session bus 与 Xorg 授权路径。
- 模式无法创建：核对独立连接器、NVIDIA ModeValidation、GDM 是否已读取新配置。
- 视频没有首帧：检查容器日志、stream 端口、X11 授权以及客户端 D3D11 解码器。
- 客户端协议 v2 必须与此服务配套；旧版“输入屏幕数量”客户端不兼容。

回退 NVIDIA 桌面时，先停用这两个服务，恢复自己备份的 Xorg/GDM/monitors.xml；保存应用后重启 GDM。PVE 端恢复备份的 VM args 时保留原有其他设备，必要时正常关机/启动 VM。不要通过卸载显卡驱动来代替恢复显示配置。
