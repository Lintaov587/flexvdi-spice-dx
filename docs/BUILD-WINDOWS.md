# Windows 构建

使用 Windows x64、MSYS2 **UCRT64**、Python 3.12+ 与 Git。以下步骤会下载源码和编译依赖；使用你分配的构建机。仓库不携带 MSYS2、编译产物或个人配置。

已验证开发环境：GCC 16.2、CMake 4.4.4、GTK 3.24.52、GStreamer 1.28.8、GLib 2.90.1。MSYS2 是滚动发行版，这些版本是验证记录，不是对未来安装版本的保证。

## 准备源码

在仓库根目录执行 PowerShell：

```powershell
python tools/fetch-sources.py --component libsoup-2.74.3 --component phodav-2.5 --component spice-gtk-0.42
git clone --branch 3.1.1 https://github.com/flexVDI/flexdp.git .build/flexdp
git -C .build/flexdp rev-parse HEAD
```

确认 flexdp 提交为 `bdf9a436e9eaa8d5aca3fdc3ae9f2d3ca24b495d`。下载工具会校验发行包 SHA256、解压并应用各自的完整补丁，遇到已有源码目录会停止，避免覆盖本地修改。离线时可以传 `--cache-dir <包含原始tar.xz文件的目录>`。

在 **MSYS2 UCRT64 Bash** 中进入仓库根目录，执行：

```bash
git -C .build/flexdp apply ../../patches/dependencies/flexdp-relocatable.patch
bash tools/windows/install-deps.sh
bash tools/windows/build.sh soup
bash tools/windows/build.sh phodav
bash tools/windows/build.sh spice
bash tools/windows/build.sh protocol
bash tools/windows/build.sh client
```

这段命令是 Bash，不能直接粘贴到 PowerShell。构建目录为 `.build/`，私有依赖前缀为 `.build/flexvdi-prefix`，客户端产物在 `.build/build-flexvdi/src/`。

客户端源码已包含补丁，不要再次对本仓库根目录应用 `patches/client`。SPICE GTK 需要使用本仓库补丁后的版本；发行版自带 DLL 没有 `remote-monitor` 和 GPU Direct 扩展。

## 增量编译

完成首次配置后，在 UCRT64 Bash 中执行：

```bash
cd .build
export PATH="$PWD/flexvdi-prefix/bin:/ucrt64/bin:/usr/bin:/usr/bin/core_perl"
export PKG_CONFIG_PATH="$PWD/flexvdi-prefix/lib/pkgconfig:$PWD/flexvdi-prefix/share/pkgconfig"
meson compile -C build-spice -j 8
meson install -C build-spice
cmake --build build-flexvdi --target flexvdi-client-bin -j 8
```

## 打包运行时

回到仓库根目录，在 PowerShell 中执行（使用自己的 MSYS2 路径）：

```powershell
python tools/windows/package.py --msys-root 'C:\msys64'
```

输出为 `dist/flexvdi-spice-dx/`。工具收集非系统 PE 依赖、GTK 资源、GStreamer 插件和许可证，遇到未解析 DLL 会失败。输出目录必须是新目录；重复打包可用 `--output` 指定另一目录。源代码仓库忽略 `dist/`。

USB 显示名称可通过 `--usb-ids <usb.ids路径>` 添加上游 [USB ID 数据库](https://github.com/usbids/usbids)。不包含开发机器的设备专用覆盖值。Windows USB 转发还需要安装相应 UsbDk 驱动。

必须保留 `libgstd3d11.dll`、`libgstvideoparsersbad.dll` 及其依赖。旧客户端使用 libsoup 2 / phodav 2，因此打包时排除依赖 libsoup 3 的 soup/adaptivedemux2 插件，避免同进程冲突。

```powershell
cd dist/flexvdi-spice-dx
.\Launch.ps1 -Dx -Windowed -SpiceUri 'spice://HOST:PORT'
```

启动器设置私有运行时路径、生成图片加载器缓存，并检查 `d3d11h264dec`。`logs/` 保存客户端诊断。没有解码器时明确报错，不降级软件解码。

## 可选诊断

| 环境变量 | 用途 |
| --- | --- |
| `SPICE_DX_STATS=1` | 每个窗口收到、提交和繁忙丢弃帧计数 |
| `SPICE_DX_INPUT_TRACE=1` | 多屏输入坐标诊断 |
| `SPICE_DX_TRACE=1` | 管线时间点诊断；旧 CPU 路径字段不等于当前端到端延迟 |
| `SPICE_DX_LOW_LATENCY=0` | 显式选择旧媒体时钟路径，包含 CPU canvas |

正常使用无需设置诊断变量。窗口标题中的 GPU Direct 表示所选路径，真正首帧成功应结合日志 `first GPU frame presented` 判断。
