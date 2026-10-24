# 小马亮度 · Xiaoma Brightness

A small native Windows tray utility for adjusting multiple monitors with one slider.

Windows 原生多显示器亮度工具：点击托盘图标展开滑块，支持拖动与滚轮微调。

## 功能与边界

- 支持硬件背光控制；不支持或控制失败的显示器使用画面调暗作为回退。
- 记住上次亮度，按显示器处理更新请求。
- 首次正常运行会登记当前用户的开机启动项；无需管理员权限。
- 支持 Windows 10/11，默认构建 x86，也可构建 x64。ARM 仿真未在本次开源准备中验证。
- 实际硬件响应取决于显示器及驱动，不能保证所有屏幕支持硬件调光。

## 构建

需要 Visual Studio C++ Build Tools 与 Windows SDK。

```powershell
.\build.ps1
.\build.ps1 -Arch x64 -OutputDirectory .\dist
```

图标已包含。若需重生成图标，需要 Python、Pillow 与 resvg-py：

```powershell
python -m pip install Pillow resvg-py
.\build.ps1 -RegenerateIcons
```

没有签名证书也可构建；可选签名参数由使用者自行提供，不包含作者证书。

## 验证

```powershell
.\scripts\test-tray-toggle.ps1
.\scripts\test-notification-icon.ps1
```

以上是托盘逻辑与通知图标身份的回归检查，不代表所有显示器兼容性验证。运行主程序会改变屏幕亮度并登记自启动；请在需要使用该工具时再启动。

## 项目结构

`src/` 为 C++ 与自制图标资源，`scripts/` 为图标生成和局部回归检查，`build.ps1` 为构建入口。

这是已有自制软件的首次公开源码整理。历史本机测试不等于社区使用量；欢迎报告可复现问题并附系统及显示器型号，不要提交个人信息或密钥。

## 许可证与依赖

本项目代码按 MIT 许可证发布。Windows SDK、Visual Studio、Python、Pillow、resvg-py 分别适用其自身许可证；不随仓库分发这些工具。仓库不包含个人运行配置、服务器连接资料、私钥、安装包或用户数据。
