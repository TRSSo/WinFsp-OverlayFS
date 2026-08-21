# OverlayFS for WinFsp (ovlfs)

这是一个基于 [WinFsp](https://winfsp.dev) 实现的 Windows OverlayFS 文件系统。它采用“上层可写 + 下层只读”的架构，类似于 Linux 下的 OverlayFS。

## ⚠️ 警告

本项目目前处于实验性状态，可能存在问题，请勿用于生产环境，请勿用于保存重要数据，因此造成的损失，概不负责。

## 核心语义

- **读取 (Read)**: 优先从顶层读取，若顶层不存在且未被隐藏，则回退到底层读取。
- **写入 (Write)**: 当首次以写入权限打开底层的文件时，会触发 **copy-up** 操作，将文件复制到顶层，后续的所有修改均在顶层进行。
- **删除/重命名 (Delete/Rename)**: 若要删除或重命名底层的条目，会在顶层创建一个特殊的 `<whiteout><名字>` 文件，用于在合并视图中隐藏底层的对应条目。
- **未实现特性**: 为了追求极致性能与简化实现，本文件系统**不实现**以下特性：ACL/安全描述符 (使用 NULL DACL)、扩展属性 (EA)、命名流 (Named Streams)、重解析点 (Reparse Points)、硬链接 (Hard Links)。

## 环境要求

- [WinFsp](https://winfsp.dev) (推荐版本 v2.2B4 以上)
- 编译: Visual Studio C/C++ 编译工具链

## 编译方法

请在 **Developer Command Prompt for VS** (VS 开发者命令行) 中执行以下命令：

```cmd
cl /utf-8 /O2 /W3 /I "%ProgramFiles(x86)%\WinFsp\inc" ovlfs.c /link /LIBPATH:"%ProgramFiles(x86)%\WinFsp\lib" winfsp-x64.lib
```

## 运行与使用

编译成功后，会生成 `ovlfs.exe`。你需要将它复制到 WinFsp 安装目录下的 `bin`。

你可以通过命令行参数指定顶层、lower 层目录以及挂载点。

```cmd
ovlfs -u <上层目录> -l <下层目录> -m <挂载点> [选项]
```

### 示例

```cmd
ovlfs -u C:\upper -l C:\lower -m O:
```

### 命令行参数说明

| 参数 | 说明 | 默认值 |
|---|---|---|
| `-u <目录>` | **必需**。指定顶层（可写层）的本地目录路径。 | 无 |
| `-l <目录>` | **必需**。指定底层（只读层）的本地目录路径。 | 无 |
| `-m <挂载点>` | 挂载点，可以是盘符（如 `O:`）或目录路径（如 `C:\mnt`）。 | 自动选择 |
| `-i <毫秒>` | 内核元数据缓存超时时间。 | `1000` |
| `-t <线程数>` | WinFsp 分发线程数（0 表示自动）。 | `0` |
| `-d <级别>` | 调试日志级别。 | 无 |
| `-D <文件>` | 调试日志输出文件（使用 `-` 输出到标准错误）。 | 无 |

## 自动化构建与测试 (CI/CD)

本项目提供了 GitHub Actions 工作流 (`.github/workflows/build.yml`)，用于在 Windows 环境下自动编译并运行 WinFsp 官方测试套件。

工作流会自动执行以下步骤：
1. 下载并校验 WinFsp 安装包 (MSI) 的 SHA256 哈希值，确保环境一致性。
2. 静默安装 WinFsp 驱动。
3. 使用 MSVC 编译 `ovlfs.c`。
4. 启动 OverlayFS 并挂载到临时目录。
5. 运行简单测试验证文件系统的正确性。
6. 卸载并清理环境。
