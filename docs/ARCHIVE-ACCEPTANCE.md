# nook解压缩：阶段 6 验收与 File Roller 对比

适用版本：**0.1-20**（0.1-19 的复核与补充见各节“0.1-20”标注）。这是自动化结果与待实机确认事项的记录，**不是“全部格式已验证、已经完全替代 File Roller”的声明**。

## 1. 已实现的默认关联流程

- 只在空窗口显示顶部横幅；关闭只影响当前窗口，“不再提示”写入偏好。
- 设置页始终保留“默认压缩软件”入口，显示已接管数量、其他主要默认程序、备份路径，以及设置／恢复按钮。
- 设置与恢复都先弹出确认框，列出声明的文件类型及它们当前的默认程序；打开、取消、启动应用均不写默认关联。0.1-20：每种类型用中文名称和扩展名列出（如“ISO 光盘映像 · .iso”）。shared-mime-info 的中文翻译标记为 `zh-Hans-CN`，Qt 和 GIO 在 zh_CN 下都匹配不到，会显示英文，所以声明的 39 种类型由程序自带名称，测试保证每种都有。
- desktop 声明当前是 52 个 MIME 名称，经本机 MIME 数据库去别名后为 39 个类型；这不是 52 种独立压缩算法。范围包括 ISO、deb/rpm、jar/apk、dmg；不接管 exe、办公文档、epub、虚拟磁盘。
- 更改前复制现有 `~/.config/mimeapps.list` 到旁边的 `.bak-nook-archive-<时间>` 文件（已存在时加编号，与 niri 配置备份同一命名方式）；同一轮重复接管不覆盖首次备份。原文件不存在时不伪造备份。
- 原默认和本应用关联基线记录在 `~/.config/nook/archive-defaults.json`。记录写入失败则不开始接管；记录损坏则报错，不覆盖它。
- 恢复只处理仍由 nook解压缩 打开的、当初确实接管的类型。原本已经是 nook 的选择和后来手动改给其他程序的类型保持原样。
- 之前没有默认程序时，只局部移除本次加入的 nook ID，保留其他程序的“打开方式”条目；不调用会重置整类型关联的 GIO reset API，不整份回滚备份。
- 原默认程序已卸载或配置无法写入时，显示错误并保留恢复记录，允许修复后重试。不会擅自清空该类型。

**没有替用户修改本机真实 MIME 默认，也没有在本阶段安装新版。** 用户安装后自行决定是否确认接管。窗口激活与打开设置时刷新状态。

## 2. 自动测试

```bash
cmake -S . -B build-undo -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=ON
cmake --build build-undo -j4
ctest --test-dir build-undo --output-on-failure -j2
```

本阶段运行结果：**15/15 组通过**。包括文件管理器和撤销回归，不只压缩模块。

- `archive-compare-checks`：8 项纯判定回归，不启动 GUI 或压缩进程；安全任务超时／失败不能通过，崩溃／无明确损坏错误／取消不能当作正确拒绝坏包，同时保留合法的单文件错误和安全跳过警告。
- `archive-defaults`：独立 XDG fixture，在首次 GIO 使用前隔离；覆盖未安装、接管、重复接管备份不变、恢复、第三方用户选择保留、初始已归 nook 的选择保留、其他打开方式条目保留、原程序卸载后重试、无法写入后重试、损坏记录拒绝覆盖。
- `archive-app`：隔离 HOME 相关 XDG 配置后创建 Qt 应用；测试横幅空状态限定、临时关闭、持久“不再提示”、设置入口常驻、打开／取消不写关联、确认接管、确认恢复、列表数量；保留密码、队列、冲突、包内修改与文件写回等原有测试。
- `archive`：原有创建格式往返、旧编码、加密、分卷、冲突、取消、损坏、安全链接和路径、Landlock、修改与转换等测试继续通过。

自动 Qt UI 测试使用 offscreen/software，能确认逻辑与布局截图，**不能证明真实 Wayland 的 GPU 动画、拖放、窗口规则和焦点体验**。

## 3. 可重复的 File Roller 对比脚本

### 依赖与运行

产品的运行／构建依赖已在 `packaging/PKGBUILD` 核对：`gio-2.0` 和 `gio-unix-2.0` 均来自 **glib2**；Qt、ICU、libarchive、7zip、libsecret 等已列入。此结论来自源码和本机包归属核对，**尚非干净 chroot 构建证明**。

对比额外需要 `python`、`file-roller`、`gtk4`（提供 `gtk4-broadwayd`）、`dbus`（提供 `dbus-run-session`）、`bubblewrap` 和可用的非特权用户命名空间。这些是开发验收工具，不因对比脚本而全部成为产品运行依赖，也不在日常 `ctest` 中强制启动。

```bash
# 快速验证：1000 个小文件 + 8 MiB 单文件
python3 tools/archive-compare.py --quick --timeout 8 \
  --output /tmp/nook-archive-compare-quick-$(date +%s)

# 默认规模：20000 个小文件 + 64 MiB 不可压缩单文件
# 放在 build-undo 下可观察实际项目所在磁盘，而非 /tmp tmpfs。
python3 tools/archive-compare.py --worker build-undo/nook-archive-worker \
  --timeout 30 --output "$PWD/build-undo/compare-$(date +%s)"

# 已安装的 worker 也可使用：--worker /usr/lib/nook/nook-archive-worker
```

输出目录必须不存在。脚本保留生成样本、JSONL 事件、stderr、File Roller 日志、内容清单、独立元数据清单及 `report.json` / `report.md`，不删除测试证据。退出码 0 代表脚本规定的必需检查通过；1 是必需检查失败；2 是依赖／隔离环境无法启动。**0 不代表下文交互待验项也通过了。**

0.1-20 修复：Broadway 的两个 UNIX socket 原先放在 `--output` 下。输出路径一长（超过 socket 地址 108 字节的上限），两个地址会被截断成同一个，Broadway 报 “Address already in use” 而无法启动，只有放在 `/tmp` 下的短路径能用。现在 socket 放在沙盒私有 `/tmp` 下的临时目录里，`--output` 可以放在任意位置。

### 隔离与判定

- 外层 bubblewrap：系统和项目只读，隐藏个人 `/home`、`/run`、`/tmp`，只开放新建结果目录写入；网络隔离。Broadway 使用私有 XDG_RUNTIME_DIR 和 UNIX HTTP socket，不开放宿主 TCP 端口。
- 内层独立 HOME、XDG、临时目录；每次 File Roller 启动用独立 D-Bus，GSettings 只存内存，禁止复用个人桌面会话。不启动浏览器，不接触密钥环或默认关联。
- 仅处理脚本生成的固定样本，不接受任意外部“攻击包”。越界目标也只指向该次运行的私有 canary。
- worker 的 stdin 始终保持打开至 `done`／超时；密码经 stdin JSON 传递。公开测试密码仅用于生成固定样本，绝不读取用户密码。
- 不能只看 worker 退出码或 `done.ok`：协议允许单文件 `error` 后继续，最终 `done.ok=true`。脚本汇总整个事件流、`done.errors` 和取消状态，CRC 样本必须判失败，损坏文件必须被移除。坏包必须有正常退出、完成事件及明确的 `corrupt` 错误；崩溃、取消或无说明的失败不算正确拒绝。
- 普通包同时与独立源清单比较相对路径、文件类型、大小、SHA-256 和链接目标；空目录也在清单内。模式、mtime、inode／硬链接关系另存元数据，**不宣称两者权限策略相同**。
- 安全样本要求 Nook：任务正常完成（允许预期的安全跳过警告）、目标外 canary 不变、无暂存残留、移除逃逸链接、保留安全链接与硬链接、跳过 FIFO、清除 set-id；File Roller 的行为只记录，不作为安全正确性的标准。
- File Roller `--force` 仅表示自动建目标目录，**不是自动覆盖或回答密码**。它没有与本脚本等价的全套无人值守交互参数。
- 加密包、坏包的参考超时明确记为 `timeout-inconclusive`；不伪装成它原生返回错误，更不当作通过。旧编码测试为 Nook 明确指定编码，而 File Roller 使用自身默认方式，差异单独标明，不比较“自动猜测准确率”。

### 本次实测

最终脚本收紧判定后重新运行默认规模，必需检查通过。报告保留在本次机器的 `/tmp/nook-archive-phase6-compare-verified/report.json` 与同目录 `report.md`；临时路径仅是本次证据，换机请按以上命令重建。

环境：File Roller 44.7、GTK 4.24.1、libarchive 3.8.10、7zip 26.05；bubblewrap 与私有 D-Bus/Broadway。生成样本，没有用户资料。

| 项目 | 结果 |
|---|---|
| 普通 ZIP、7z、tar.gz、tar.xz | 双方完成，路径／类型／大小／SHA-256 与源一致，包含中文、空格、emoji、空文件、空目录 |
| 20000 个 64 字节小文件 ZIP | 双方完成，源清单一致 |
| 64 MiB 不可压缩单文件 ZIP | 双方完成，源清单一致 |
| GB18030、Big5、Shift_JIS ZIP | Nook 指定编码后名称与源一致；本次 File Roller 默认解出的名称不同，不记作编码对齐 |
| 加密文件名 7z | Nook 经 stdin 提供公开样本密码后内容一致；File Roller 8 秒未退出，已回收，交互待验 |
| CRC 错误 ZIP、截断 ZIP | Nook 可解释地报错，CRC 错误文件移除；File Roller 8 秒未退出，交互待验 |
| 路径越界、链接逃逸、安全链接、硬链接、set-id、FIFO | Nook 按文档安全契约检查；目标外 canary 保持，File Roller 不作为安全标准 |

报告内耗时只作复现参考：**单次启动到退出，含私有 D-Bus／GTK 启动开销，不含样本构造和哈希扫描**。`/tmp` 本机为 tmpfs，缓存未控制，等待进程的采样也有精度限制；不能据此宣传速度倍数或“无性能瓶颈”。正式性能验收应在实际文件系统上预热、交替顺序至少三轮，报告中位数、内存峰值和缓存／落盘条件；GiB 级及更多文件的测试仍待执行。

### 大包性能（0.1-20）

```bash
# 每个大包解开约 1 GiB；输出放在实际磁盘上（不要放 tmpfs），需约 6 GiB 空间
python3 tools/archive-compare.py --worker build/nook-archive-worker --bench --bench-gib 1 --rounds 3 \
  --timeout 1200 --output "$PWD/build/bench-$(date +%s)"
```

`--bench` 生成四个大包：1 GiB 不可压缩 ZIP（8 个文件）、100000 个小文件 ZIP、1 GiB 可压缩 7z 和 tar.zst。每轮先把压缩包完整读一遍，两边都在热缓存下运行；每轮交替先后顺序，解压到同一文件系统上的空目录。记录两项时间：工具返回所用的时间，以及再加上对输出执行 `sync -f`（落盘）的时间。峰值内存是整个进程树的最大 RSS（wait4，包含 worker 调用的 7-Zip、`dbus-run-session` 下的 File Roller），每次的内容都与源清单比对。

本机实测（btrfs、NVMe、16 线程、46 GiB 内存；3 轮全部完成且内容一致；中位数）：

| 压缩包 | 压缩后 | 文件数 | Nook 用时（含落盘） | Nook 峰值内存 | File Roller 用时（含落盘） | File Roller 峰值内存 |
|---|---|---|---|---|---|---|
| large.zip（不可压缩） | 1024 MiB | 8 | 0.33 s（0.39 s） | 23 MiB | 0.44 s（0.46 s） | 58 MiB |
| many.zip | 119 MiB | 100000 | 1.22 s（1.29 s） | 49 MiB | 2.13 s（2.20 s） | 207 MiB |
| large.7z | 342 MiB | 8 | 0.72 s（0.76 s） | 355 MiB | 0.82 s（0.86 s） | 354 MiB |
| large.tar.zst | 329 MiB | 8 | 0.82 s（0.86 s） | 23 MiB | 1.48 s（1.53 s） | 62 MiB |

解读：

- 解压 1 GiB 级压缩包时，两边都没有出现失败、内容不一致或内存随大小增长的情况；Nook 的 worker 内存基本固定（ZIP、tar 约 23 MiB），十万个小文件时也不到 50 MiB。7z 两边都调用 7-Zip，内存由 7-Zip 的字典决定，所以相同。
- 量的是后台进程（worker）对比 File Roller 整个程序。File Roller 的时间里包含约 0.1 s 的 GTK/Broadway 启动；Nook 界面本身的开销没有计入。所以这只说明“大包下没有明显瓶颈、内存可控”，**不作为速度倍数的宣传**。
- 只测了一台机器、热缓存；冷缓存、机械硬盘、更大的包（10 GiB 以上）和内存紧张时的表现仍待测。

## 4. 真实桌面验收清单（待用户实机确认）

以下未由 offscreen 或 CLI 结果替代。只在用户愿意的测试配置／资料副本中操作。

### 窗口与外观

- [ ] 从启动器独立打开“nook解压缩”，空窗口无压缩包也可新建／打开。
- [ ] niri 规则使窗口浮动、尺寸接近 Nook 文件选择器，不平铺；新建／解压／设置／任务窗也验证。
- [ ] 浅／深色、字体、缩放 0.75–1.5 与 Nook 一致；最小窗和长路径不遮挡确认按钮。
- [ ] 真 GPU 下滚动、弹窗、任务切换与进度更新流畅；高负载没有主线程明显卡顿。
- [ ] 键盘焦点、Esc、Enter、快捷键、模态层与输入法正确，不穿透弹窗。

### 默认关联（真实更改必须由用户确认）

- [ ] 空窗口横幅临时关闭；新窗口仍显示；“不再提示”重启仍生效；设置入口始终可用。
- [ ] “设为默认…”只打开确认框；取消后用 `xdg-mime query default application/zip` 等检查没有变化。
- [ ] 确认前理解包含 ISO、软件包、jar/apk/dmg；确认后检查备份、ZIP／7z／RAR／ISO 等关联及双击启动。
- [ ] 把一类手动改给第三个程序，然后恢复；该类保留，其他仍归 Nook 的恢复，原本已属 Nook 的保留。
- [ ] 原程序卸载或目录不可写时能看到错误，恢复记录保留，修复后重试成功。

### Nook 与桌面集成

- [ ] 右键“压缩…”打开参数窗，选中文件／文件夹和输出路径正确。
- [ ] “解压到当前文件夹”按已确定的策略落盘；“解压到…”打开 Nook 选择器。
- [ ] 多个选中压缩包、分卷选择、拖入／拖出、打开包内文件及写回提示在真实 Wayland 可用。
- [ ] 后台任务、关窗继续、取消与重新打开、通知及任务冲突／密码窗定位正确。
- [ ] 密钥环锁定、解锁、保存密码、错误密码重试及关闭后清理符合设置。

### 格式、故障与边界

- [ ] 使用合法来源的真实 RAR／加密 RAR／分卷 RAR；不能用改后缀的 ZIP 代替。
- [x] sv4crc、rpm、cab、lha（0.1-20，`archive` 测试里按格式规范逐字节生成的最小样本：带校验的 SVR4 cpio、空签名和空头部的 RPM（内含 gzip 压缩的 cpio）、不压缩的 CAB、存储方式（lh0）的 LHA；这些样本都能列出、解压，系统识别出的类型也在声明的类型里）。
- [ ] 真实世界的压缩变体：CAB 的 MSZIP/LZX、LHA 的 lh5/lh6/lh7、带完整头部的真实 RPM。
- [ ] arj、dmg、squashfs：本机无法生成，需要真实样本；没有样本的不可标通过。
- [ ] File Roller 密码、覆盖与错误对话框实机作同任务比较，关闭提示后记录原生完成／失败结果。
- [ ] 磁盘满、只读位置、目标设备拔出、进程强杀后暂存清理、大文件和多文件内存峰值。

## 5. 范围边界与后续决策

本阶段延续既有实现，不重写压缩引擎、队列、包内修改或文件撤销。默认关联审查发现的局部数据安全问题已修补并加回归测试。

未验证的长尾格式、真实桌面交互、GiB 级持久化性能不是小补丁能够凭空证明的事项；需要样本和用户实测，不能借 MIME 声明代替兼容性验证。远程位置、额外长尾压缩后端等仍按已确认的分期范围处理。若实测暴露引擎级或明显范围扩大的缺口，先列明差异再讨论，不在本阶段随意改动旧逻辑。
