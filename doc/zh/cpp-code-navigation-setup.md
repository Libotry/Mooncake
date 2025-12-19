# C++ 代码导航设置指南

## 问题

如果 IDE（如 VS Code、CLion、Vim 等）无法跳转到 C++ 代码定义，通常是因为缺少 `compile_commands.json` 编译数据库。

## 解决方案

### 方法 1：使用脚本自动生成（推荐）

#### Windows

```powershell
# 在项目根目录执行
.\scripts\generate_compile_commands.bat
```

#### Linux/macOS

```bash
# 在项目根目录执行
chmod +x scripts/generate_compile_commands.sh
./scripts/generate_compile_commands.sh
```

### 方法 2：手动生成

#### 步骤 1：创建构建目录并配置 CMake

```bash
# Windows PowerShell
mkdir build
cd build
cmake .. -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# Linux/macOS
mkdir -p build
cd build
cmake .. -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

#### 步骤 2：复制 compile_commands.json 到项目根目录

```bash
# Windows PowerShell
copy compile_commands.json ..

# Linux/macOS
cp compile_commands.json ..
```

### 方法 3：如果 CMake 未安装

1. **安装 CMake**：
   - Windows: 从 [CMake 官网](https://cmake.org/download/) 下载安装
   - Linux: `sudo apt-get install cmake` (Ubuntu/Debian) 或 `sudo yum install cmake` (CentOS/RHEL)
   - macOS: `brew install cmake`

2. 然后按照方法 1 或方法 2 执行

## 验证

生成成功后，项目根目录应该有一个 `compile_commands.json` 文件。

### VS Code

1. 安装扩展：
   - **C/C++** (Microsoft)
   - **clangd** (可选，更强大的语言服务器)

2. 重启 VS Code，代码跳转应该可以正常工作

### CLion

CLion 会自动检测 `compile_commands.json`，无需额外配置。

### Vim/Neovim

如果使用 **coc-clangd** 或 **nvim-lspconfig**，确保已安装 clangd：

```bash
# macOS
brew install llvm

# Linux
sudo apt-get install clangd

# Windows
# 从 LLVM 官网下载安装
```

## 已创建的配置文件

项目根目录已包含以下配置文件：

- **`.clangd`**: clangd 语言服务器配置
- **`scripts/generate_compile_commands.sh`**: Linux/macOS 生成脚本
- **`scripts/generate_compile_commands.bat`**: Windows 生成脚本

## 故障排除

### 问题 1：CMake 配置失败

检查是否安装了所有依赖：
- C++ 编译器（GCC/Clang/MSVC）
- CMake 3.16 或更高版本

### 问题 2：生成的文件太大或不完整

某些 CMake 配置可能不会为所有目标生成编译命令。可以尝试：

```bash
cd build
cmake .. -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_BUILD_TYPE=Debug
```

### 问题 3：IDE 仍然无法跳转

1. 确保 `compile_commands.json` 在项目根目录
2. 重启 IDE
3. 检查 IDE 的 C++ 扩展是否已安装并启用
4. 查看 IDE 的输出日志，查找错误信息

## 更新编译数据库

如果修改了 CMakeLists.txt 或添加了新文件，需要重新生成：

```bash
# 重新运行生成脚本
./scripts/generate_compile_commands.sh  # Linux/macOS
# 或
.\scripts\generate_compile_commands.bat  # Windows
```







