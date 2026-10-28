# 输入法与候选框样式

nook 使用系统输入法，不内置候选框，不强制选择某个输入法，也不打包或修改用户的 Fcitx 主题。

## 本次灰色候选框的原因

在 niri / Wayland 下，fcitx5-qt 可以在 Qt 应用进程内绘制候选框。插件读取当前进程可见的 `fcitx5/conf/classicui.conf`，再从用户数据目录查找主题与图片；不能假设已经运行的 Fcitx 服务会负责所有候选框的绘制。

开发时使用临时 `XDG_CONFIG_HOME` / `XDG_DATA_HOME` 隔离 nook，会同时隐藏 Fcitx 的配置与主题。插件因此退回默认的灰色样式。恢复主题文件可见性后，已通过实机候选框对比确认恢复用户现有主题。

0.1 增加只隔离 nook 自身数据的开发配置目录，后续实机验证与性能测量使用这种方式，不再改动桌面级 XDG 目录：

```bash
./build/nook --profile-dir /tmp/nook-review --no-index /path/to/demo
```

此目录内的 `config/preferences.ini`、`data/index.sqlite`、`cache/` 与个人配置独立。开发实例不会转发给普通实例，不写系统自启动入口，不在关闭窗口后后台常驻。预览子进程沿用同一个 nook 配置目录。也可用 `NOOK_PROFILE_DIR` 环境变量指定；启动后会消费该变量，不会把开发配置目录传给从 nook 打开的终端或其他应用。

不传入开发配置目录时，原有配置与索引位置不变。

## 其他应用为什么也可能不同

- Qt 应用需要可用的 Fcitx Qt 插件。部分 Wayland 接入由客户端绘制候选框，必须能访问系统主题配置。
- GTK、Electron、浏览器可能通过不同的 Wayland 输入法协议接入；应用自带运行时也可能缺少匹配的输入法模块。
- Flatpak 等沙箱可能看不到用户目录中的主题；只设置环境变量不一定能解决。
- 应用自绘的搜索建议、自动补全列表不是输入法候选框，不能套用 Fcitx 皮肤。

因此，统一方向是让应用沿用系统 Fcitx 与当前主题，而不是给每个应用重新画一套候选框。不要为解决某个应用而一律覆盖全局 `GTK_IM_MODULE`、切换到 XWayland，或重置用户主题；应先核实它实际使用的输入法通道和主题可见性。

本机 Qt 的 `QT_IM_MODULE=fcitx` 与 Fcitx Qt/GTK 集成包已配置；本次没有更改全局输入法设置、niri 配置或用户主题。未逐一验证其他第三方应用，不能保证它们都已统一。

参考：fcitx5-qt 5.1.16 的 [主题加载实现](https://github.com/fcitx/fcitx5-qt/blob/5.1.16/qt5/platforminputcontext/fcitxtheme.cpp) 和 [Qt 输入法接入](https://github.com/fcitx/fcitx5-qt/blob/5.1.16/qt5/platforminputcontext/qfcitxplatforminputcontext.cpp)。
