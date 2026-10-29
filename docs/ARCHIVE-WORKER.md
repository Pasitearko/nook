# nook解压缩后台进程（nook-archive-worker）

`nook-archive-worker` 是普通用户权限的独立进程，读写压缩包。界面程序通过 `ArchiveJob`（`src/archive/archivejob.h`）启动它，每个任务一个进程。安装位置：`/usr/lib/nook/nook-archive-worker`。

## 协议

- stdin 第一行是 JSON 请求；之后每行是一条命令。stdin 关闭视为取消（客户端已不在，没人能回答问题或看到结果）。
- stdout 每行一个 JSON 事件，最后一个总是 `done`。
- 密码只经 stdin 传递，不出现在命令行、环境变量或日志里。

### 请求

| `op` | 字段 |
|---|---|
| `list` | `archive`，`encoding`（`auto` 或 `UTF-8`、`GB18030`、`Big5`、`Shift_JIS`、`EUC-JP`、`EUC-KR`、`IBM437`、`IBM866`、`windows-1251`、`windows-1252`、`KOI8-R`、`ISO-8859-1`），`password` |
| `test` | 同上 |
| `extract` | 同上，加 `destination`；`entries`（只解压这些路径，目录含其内容）；`prefix`（去掉的上层路径，例如在包内子文件夹里解压选中项）；`flatten`（不保留目录）；`subfolder`：`smart`（默认，多个顶层项目时才建文件夹）、`always`、`never`；`folderName`（默认为压缩包名去掉后缀）；`conflict`：`ask`（默认）、`overwrite`、`skip`、`rename`；`expectedSize`（用于事先检查磁盘空间） |
| `create` | `format`（`zip`、`7z`、`tar`、`tar.gz`、`tar.bz2`、`tar.xz`、`tar.zst`、`gz`、`bz2`、`xz`、`zst`），`output`，`items`，`password`，`conflict`，`testAfter`，`options` |

`create` 的 `options`：`level`（7z/ZIP 0～9；gzip/xz 0～9；bzip2 1～9；zstd 1～22）、`method`（7z：LZMA2、LZMA、PPMd、BZip2、Deflate、Copy；ZIP：Deflate、Deflate64、BZip2、LZMA、PPMd、Copy）、`dictionary`（字节）、`wordSize`、`solid`（布尔或字节）、`threads`、`volumeSize`（字节，仅 7z）、`encryptNames`（7z，默认开）、`zipEncryption`（`AES256` 默认，或 `ZipCrypto`）、`exclude`（通配符数组）。

### 命令

- `{"cmd":"password","value":"…"}`：回答 `need-password`。
- `{"cmd":"resolve","id":N,"choice":"overwrite|skip|rename","all":true}`：回答 `conflict`；`all` 对之后的冲突沿用。
- `{"cmd":"cancel"}`。

### 事件

| 事件 | 内容 |
|---|---|
| `started` | `sandbox`：Landlock 是否生效 |
| `info` | `backend`、`format`、`filters`、`encoding`（实际使用的编码）、`legacyNames`、`encrypted`、`headerEncrypted`、`solid`、`split`、`volumes`、`stream`（单文件压缩流） |
| `entries` | 每批最多 1000 项：`path`、`type`（`file`、`dir`、`link`、`hardlink`、`other`）、`size`（未知为 −1）、`packed`、`mtime`、`mode`、`link`、`encrypted`、`method`、`unsafe`（路径越界） |
| `staging` | 暂存目录路径；客户端在 worker 被强行终止时删除它 |
| `progress` | `phase`（`list`、`test`、`extract`、`commit`、`create`）、`fraction`、`bytes`、`files`、`current`；最多每 100 ms 一次 |
| `need-password` | `wrong`：上一个密码被拒绝 |
| `conflict` | `id`、`path`（相对目标文件夹）、`existing` 与 `incoming`（`size`、`mtime`、`directory`） |
| `warning` | `code`（`unsafe-path`、`unsafe-link`、`special`、`hardlink`、`renamed`、`kept-folder`、`io`、`read`、`write`、`create`）、`path`、`message` |
| `error` | 单个文件出错，任务继续 |
| `done` | `ok`、`cancelled`、`error`（`code`、`message`）、`created`（相对目标文件夹，供“删除本次已解压的内容”）、`skipped`、`destination`；`list` 另有 `count`、`totalSize`；`test` 另有 `errors`、`tested`；`create` 另有 `output` |

`error.code`：`not-found`、`missing-volume`、`unsupported`、`corrupt`、`wrong-password`、`no-space`、`destination`、`options`、`test-failed`、`memory`、`missing-tool`、`failed`，以及客户端补充的 `missing-worker`、`crashed`。

7-Zip 后端的 `entries` 可能先于 `info` 到达。

## 后端

见 `docs/ARCHIVE-DESIGN.md` 第 16 节。按文件头和文件名选：7z、RAR、WIM 和分卷用 7-Zip，其余用 libarchive；libarchive 打不开或遇到不支持的压缩方式时改用 7-Zip。

## 写盘与安全

1. **Landlock**：请求解析后，worker（及其启动的 7-Zip）只能写目标文件夹（解压）或保存位置（创建），且不能创建设备、管道和套接字；列表和测试完全不能写。读取不受限。内核不支持时照常工作，`started.sandbox` 为 `false`。
2. **暂存**：解压先写入目标文件夹里新建的 `.nook-extract-*`（0700），全部完成后再移入；创建先写入 `.nook-create-*`，校验（如要求）通过后再改名。中途失败、取消或被杀，半成品都留在暂存目录里，由 worker 或客户端删除。
3. **路径**：含 `..` 的条目跳过；绝对路径去掉开头的 `/`；ZIP 的 `\` 视为目录分隔符。逐级用 `O_NOFOLLOW | O_DIRECTORY` 打开目录，任何一级是符号链接就跳过该条目，因此不会经由包内的链接写到外面。
4. **链接**：解压后逐个解析暂存区里的符号链接（包括经过其他链接的情况），指向解压位置之外或使用绝对路径的删除并警告。硬链接只能指向本次已解压的文件。
5. **权限**：恢复修改时间和 rwx 权限（再套用 umask），不恢复所有者和 setuid、setgid、sticky 位；目录总保留属主读写执行；设备文件、管道和套接字不解压。
6. **同名**：包内同一路径重复出现时，后出现的覆盖前面的；不同条目（例如不同编码）解码成同一名称时自动改名。移入目标时同名文件夹合并，同名文件按 `conflict` 处理；文件不会替换文件夹。
7. **空间**：已知解压后大小时先检查剩余空间（另留 64 MB）；libarchive 后端写入过程中每 32 MB 再查一次，7-Zip 后端以它报告的磁盘已满为准。
8. **取消**：删除正在写的文件，已完整解出的移入目标（遇到同名一律跳过，不再询问），`done.cancelled` 为 `true`；创建任务取消后不留任何文件。

## 文件名编码

worker 以 `C.UTF-8` 运行，libarchive 把未标记 UTF-8 的 ZIP 文件名原样交出。`auto` 时，本身是合法 UTF-8 的名称按 UTF-8 解码；其余用 ICU 检测，检测不可靠时按桌面语言回退（简体中文为 GB18030，繁体中文 Big5，日文 Shift_JIS，韩文 EUC-KR，俄文 IBM866，其他 IBM437）。指定编码时对所有非 ASCII 名称使用该编码，便于预览切换。

## 测试

`tests/archive_test.cpp`（ctest 名 `archive`）覆盖：分卷与命名规则、编码检测、旧编码 ZIP、智能文件夹、冲突处理、路径穿越与链接逃逸、set-id 位、硬链接、特殊文件、ZIP AES 密码重试、所有可创建格式的往返、7z 文件名加密、分卷创建与解压、选中项/前缀/平铺、取消、损坏检测、Landlock 与被杀后的清理。需要本机安装 `7z`，否则相关用例跳过。尚无 RAR 样本（本机无法创建 RAR），RAR 读取依赖 7-Zip，待真实样本验证。
