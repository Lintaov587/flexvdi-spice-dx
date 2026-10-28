# 验证记录

记录日期：2026-10-28。下面区分已运行功能与源码发布整理，不能将源码检查等同于新环境端到端验证。

## 已运行客户端与客户机

Windows x64 / RTX 5060 Laptop；Ubuntu GNOME/X11 / RTX 5090 D，驱动 580.178.04；PVE KVM。

- 已批准的独立本地 MSYS2 中完成 SPICE GTK 的 Meson 与 flexVDI 的 CMake 编译。
- GNOME/Xorg 的默认 `glxinfo -B` 返回 NVIDIA RTX 5090 D，无需逐应用 offload 变量。
- 单路 H.264 使用 `d3d11h264dec`，NV12 decoder-bound 纹理经 VideoProcessorBlt 写入独立交换链；日志确认首帧呈现、逐帧 CPU copies=0、video timers=0。
- 四块 1080p 虚拟屏及四个独立窗口已创建；四块 720p 的四个交换链均完成 600 次提交、busy=0。
- 两块 1600×900 各提交 2700/2700 帧、busy=0；某个 32 秒区间约 37fps。这是帧提交统计，不是端到端延迟测量，也不是所有场景的帧率保证。
- 设备级 `SetMaximumFrameLatency(1)` 导致副屏卡顿的问题已重现；改为各交换链 `FRAME_LATENCY_WAITABLE_OBJECT` + `IDXGISwapChain2::SetMaximumFrameLatency(1)` 后上述统计恢复正常。没有等待该事件对象，也没有增加刷新定时器。
- 第四屏输入坐标与客户机 XQueryPointer 相差约 1 像素，符合虚拟输入归一化；原生绘制子窗口返回 HTTRANSPARENT。
- 历史缩略图保存成功。WebDAV 使用 `Depth: 1` 的 PROPFIND 返回 HTTP 207。

## 显示器管理 v2 实测

通过真实客户端管理界面完成：

1. 在 1184×714 主屏旁添加 1282×720 自动副屏。
2. 将副屏外窗拉伸为 1100×780，只有其客户机分辨率变为 1084×694，主屏不变，未出现持续往返调整。
3. 将副屏修改为固定 1086×694，再拉伸外窗到 1300×850，远程分辨率保持不变。
4. 删除 id=0 主屏，id=1 的原窗口继续存在并成为主屏；重新添加 id=0 后，编号与窗口保持对应。
5. 删除正在承载管理对话框的副屏，对话框与副屏窗口正常关闭。
6. 恢复一块 1600×900 自动屏，正式客户端重连成功，600/600 帧提交、busy=0，两个客户机服务均 active。

## 本次源码发布检查

- SPICE GTK、libsoup、phodav、streaming-agent 补丁重新从原始发行包生成，发行包 SHA256 已校验。
- 所有四个发行包的 `git apply --check` 与实际应用均已通过。
- 补丁应用后的所有变更文件与已部署开发源码一致；客户端补丁在干净 3.1.7 上应用后与公开源码一致。
- 新服务文件 patch 已在空目录应用并逐文件核对；README 与部署文档的相对链接均可解析。
- 客户端完整源码保留上游 3.1.7 历史。公开仓库去掉开发机内网地址示例，不上传运行日志、连接历史、私有 SSH 信息或编译环境。
- Windows 构建脚本改为仓库相对目录，发布打包脚本采用已验证运行包的 GStreamer 插件集合。
- 发布打包脚本已使用现有编译产物成功收集并解析 268 个 PE 二进制的依赖；运行包留在忽略的 dist 目录，不随源码上传。
- 服务端公开 Dockerfile 改用 `debian:bookworm-slim` 并列出依赖；启动配置移除开发用户与固定工作目录，改用 `/etc/default/spice-fullscreen`。

本次发布整理没有重启生产桌面，也没有在干净机器上重建完整 Windows 依赖和公开 Docker 基础镜像。这些通用构建/部署入口不能宣称已在任意新机器上验证成功。运行路径的验证来自上面已经部署的版本。

## 已知范围

AMD/Intel Windows 客户端完整链路、Wayland、其他桌面环境未验证。USB 已配置驱动和四个转发通道，但本次多屏验证没有重新选择并转发实体设备。旧音频属性、Windows session DBus 和偶发 cursor/USB focus 日志仍存在，不属于本次显示管理改动。

没有把此前单屏管线的历史延迟数字当作 NVIDIA 多屏版本的延迟结论。
