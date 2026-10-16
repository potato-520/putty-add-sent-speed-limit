# PuTTY-WebView 界面与 MSI

## 使用

安装 `dist/PuTTY-WebView-1.0.3-x64.msi`。安装范围为当前用户，默认目录为
`%LOCALAPPDATA%\Programs\PuTTY-WebView`。安装界面可以选择其他路径，
桌面和开始菜单默认都添加快捷方式。

默认 AppData 是为了当前用户安装，无需管理员权限；这也是程序安装目录，
不是临时解压目录。可以选择有写入权限的其他位置，例如 `D:\Tools\PuTTY-WebView`。
`Program Files` 通常需要管理员权限；当前包保持当前用户安装范围，不会自动请求提权。
需要本机已有 Microsoft Edge WebView2 Runtime；MSI 不联网下载运行库。

便携运行时，必须把 `build-vs/Release/webview` 文件夹和
`build-vs/Release/putty_webview.exe` 一起复制。EXE 不再包含、解压或更新网页资源；
缺少资源时提示重新安装/修复，也不再回退到源码目录或历史解压缓存。

白色系统标题栏由 30px 深色标题栏替代，支持拖动、双击最大化/还原、右键系统菜单，
提供最小化、最大化和关闭按钮。原生缩放边框仍保留。
标签改为扁平样式、Segoe UI 字体、细分隔线和蓝色选中下划线，保留标签拖动、拆窗及分屏。

## 编译

```powershell
.\build.bat putty_webview
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-msi.ps1 -Version 1.0.1
```

WiX 3.14.1 工具位于 `.tools/wix3`；该目录不入 Git。
工具缺失时，从 [WiX 官方发布页](https://github.com/wixtoolset/wix3/releases/tag/wix3141rtm)
下载 `wix314-binaries.zip` 并解压到该目录，或者给脚本传入 `-WixDir`。
已有最新 EXE 和资源时可使用 `-SkipBuild`。
MSI 自动包含当前完整资源目录，并调用已有 `sign_putty.ps1` 签名。
版本采用 MSI 的三段格式，发布新版建议递增版本号。

资源同步目标每次构建都会刷新 `build-vs/Release/webview/web`，
所以仅修改 HTML/CSS 时也会更新输出，并清除构建目录内的过期文件。
`sync_assets.cmake` 在删除前检查目标必须位于当前构建目录内，且为专用资源目录。

## 重装及旧资源清理

固定 UpgradeCode，每个新编译的 MSI 生成新 ProductCode。
同版本重编译的 MSI 也按大版本升级处理；先移除旧产品，再安装新产品。
移除发生在事务开始后，失败时可回滚旧产品。

[WiX RemoveFolderEx](https://docs.firegiant.com/wix3/xsd/util/removefolderex/)
在安装、修复和卸载时递归清理安装目录内专用的 `webview` 树，
然后由 MSI 安装当前资源。这包括不再被新版包含的目录及未登记的旧资源。
直接再次打开完全相同的 MSI 时进入维护界面，选择 Repair。
资源目录属于安装器，请勿在里面保存个人文件。
资源目录以外的文件和 PuTTY 会话配置不由这项清理删除。
历史便携版/旧自解压版所在的其他目录不会被新 MSI 自动删除，也不会被程序加载。

安装路径会写入 HKCU 注册表，升级、修复、卸载会恢复实际路径。
旧资源清理使用实际 INSTALLDIR 下的 webview，不再使用固定 AppData 路径。
升级默认沿用上次路径；可以在新安装包的目录选择页更改。
静默安装也可传入 `INSTALLDIR="D:\Tools\PuTTY-WebView"`。
修复命令：

```powershell
msiexec /fa "dist\PuTTY-WebView-1.0.3-x64.msi"
```

## 验证记录（2026-10-07）

- Release 编译完成，EXE 与 MSI 的本机 Authenticode 验证状态均为 Valid。
- 浏览器渲染检查：两个标签正常，标签无圆角，窗口按钮发送正确宿主消息，无 JavaScript 异常。
- 隔离测试 MSI（独立产品名、UpgradeCode 和安装路径）安装/升级/修复返回 0。
- 升级删除嵌套旧资源目录、替换被修改的 index.html，安装后资源数量为 244。
- 修复同样清除旧目录并恢复 HTML；资源目录外的测试文件保留。
- 同版本重编译覆盖安装返回 0，244 个资源文件逐一比对哈希，全部匹配；嵌套旧资源被清理。
- 隔离测试包卸载后清理测试安装目录，正式包未安装到本机。
- Windows 应用控制策略返回 4551，阻止实际 EXE 启动；原生拖动、缩放及按钮行为尚未实机验证。
- WiX ICE 验证因系统策略无法执行（LGHT1105）；未关闭策略或取消验证。

移除自解压可以减少运行时释放文件行为，但不代表 ESET 或 Windows 应用控制一定放行。
本机已有证书签名也不等于系统策略认可的公开发布者签名。

原生拖动区域使用 [WebView2 非客户区支持](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2settings9)，
旧运行库使用宿主消息回退。

## 1.0.2 安装器更新

新增默认桌面快捷方式及安装目录选择页，记录实际安装位置以供升级、修复和卸载清理。

验证：包含空格的自定义目录安装成功，桌面快捷方式目标正确；
不传路径的修复恢复原安装位置并清理嵌套旧资源；
升级改到新目录后，原目录的资源被清除，资源外测试文件保留，新目录全部 244 个资源哈希匹配。
卸载无需传路径，能清理新安装目录和桌面快捷方式。
ICE 验证仍被系统策略阻止（LGHT1105）。

## 1.0.3 浮动编辑器拖动修复

浮动编辑器的顶部位置限制在主窗口标题栏下方，重新打开和居中同样遵循该边界。
编辑器及其子控件显式设为 app-region: no-drag，避免与原生整窗拖动区域冲突。
浏览器回归测试：向上拖动停在 y=30，再次向下拖动到 y=130，关闭重开位置正常，
没有发送整窗拖动消息，没有 JavaScript 异常。原生 WebView2 行为未在本次测试中验证。
