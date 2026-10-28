# 补丁基线与应用

所有上游基线、下载地址和 SHA256 见根目录 `sources.lock.json`。本目录描述的是完整发行补丁，不依赖开发过程中的中间快照。

| 补丁 | 基线 | 内容 |
| --- | --- | --- |
| `patches/client/flexvdi-3.1.7-dx.patch` | flexVDI Client 3.1.7，`a818de7b9f69464899560a22aa6dbb43d5c3d2d3` | Windows 构建、连接历史、DX 选择、独立窗口、协议 v2 显示器管理 |
| `patches/spice-gtk/0001-windows-gpu-direct.patch` | SPICE GTK 0.42 原始 tar.xz | 导出符号、USB stdcall、GPU 解码/呈现、H.264 协商、多屏裁切与输入映射 |
| `patches/dependencies/libsoup-windows.patch` | libsoup 2.74.3 | Windows setsockopt 参数类型 |
| `patches/dependencies/phodav-windows.patch` | phodav 2.5 | Windows 不使用 ELF version script |
| `patches/dependencies/flexdp-relocatable.patch` | flexdp 3.1.1，`bdf9a436e9eaa8d5aca3fdc3ae9f2d3ca24b495d` | pkg-config include 路径可重定位 |
| `server/patches/spice-streaming-agent-fullscreen.patch` | spice-streaming-agent 0.3 原始 tar.xz | X11 整屏 H.264、尺寸变化处理、流插件与日志 |
| `server/patches/guest-display-services.patch` | 空目录，无上游基线 | 新增 NVIDIA 多屏控制、EDID、服务配置、容器与 PVE 配置样例 |

## 从上游客户端应用

在独立目录检出上游客户端的精确提交，先检查，再应用：

```bash
git clone --branch 3.1.7 https://github.com/flexVDI/flexvdi-client.git flexvdi-client-dx
cd flexvdi-client-dx
git apply --check /absolute/path/to/flexvdi-3.1.7-dx.patch
git apply /absolute/path/to/flexvdi-3.1.7-dx.patch
```

本仓库 `src/` 与 `resources/` 已经应用这些改动，因此无需重复应用客户端补丁。

## 从上游 SPICE GTK / streaming-agent 应用

下载精确版本并校验 SHA256，然后进入解压出的源码根目录：

```bash
patch --dry-run -p1 < /absolute/path/to/the-component.patch
patch -p1 < /absolute/path/to/the-component.patch
```

`tools/fetch-sources.py` 自动完成下载、校验、解压和 `git apply --check` / `git apply`。它在每个解压目录初始化独立 Git 元数据，避免从主仓库子目录应用补丁时发生路径过滤。

服务端 `guest-display-services.patch` 包含新增文件：在一个**空的独立部署目录**用 `patch -p1` 应用后，会生成 `server/nvidia`、`server/streaming`、`server/pve`。这份补丁不是对 `/etc` 的修改，不会自动安装服务；仍需按 [SERVER.md](SERVER.md) 核对用户、设备和会话后部署。Docker 构建还需要本仓库下载的上游归档和 streaming-agent patch。

## 各部分为什么都需要

客户端的 `remote-monitor` 属性依赖修改后的 SPICE GTK，不能只更新 flexVDI EXE。服务端控制协议为 **v2**，每个显示器都有稳定 id、独立尺寸及 auto_resize 标志；请求以 request_id 匹配响应。旧版按“数量+统一分辨率”配置的客户端不能与 v2 混用。

显示器控制服务将 Mutter 的输入显示器索引传给客户端；它与 SPICE 视频通道编号不同。GPU 呈现对每个交换链分别设置帧延迟上限，不能改回设备全局上限 1，否则副屏可能被主屏挤占呈现机会。

没有包含未部署的 QEMU 或 spice-server 试验补丁。宿主配置变化在 `server/pve/qemu-args.example`，实际视频编码修改在 streaming-agent patch。
