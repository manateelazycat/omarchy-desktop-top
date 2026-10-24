# Omarchy Desktop Top

简体中文 | [English](README.md)

沿用 [Omarchy Desktop Clock](https://github.com/manateelazycat/omarchy-desktop-clock) 外观的桌面 btop 插件。直角面板、半透明背景、细边框、四角装饰和字体跟随 Omarchy 主题，内部是可交互的真实终端。

https://github.com/user-attachments/assets/897d885b-979f-4844-ae5e-6cfc77114b2a

[原始帖子](https://x.com/manateelazycat/status/2106797188567519713)

## 功能

- 每个显示器只在当前空工作区显示；应用窗口、固定窗口或打开的特殊工作区内的窗口使该显示器上的插件隐藏。
- 完全位于屏幕外的窗口不影响空工作区判断；部分仍在屏幕内的窗口继续使插件隐藏。
- 标题和外框留白可拖动位置；四条边和四个角可拖拽调整大小，光标提示缩放方向。
- 调整大小改变终端行列数，btop 自动重新布局，字体保持原大小。
- 默认 CPU、内存、网络、进程四面板的最小内容区为 **80 列 × 24 行**。最小窗口宽高由字体实际字符尺寸加上外框计算，无法拖到更小。
- 共享相对位置和逻辑像素尺寸，在松开鼠标时保存一次，登录后恢复；小显示器限制窗口大小，保持完整可见。
- 点击右上角最小化按钮，窗口收起为屏幕顶部的小黑条，中间短线使用当前主题强调色；点击黑条恢复原位置、大小及同一个 btop 进程。各显示器分别保存收起状态，插件重载和切换工作区后保留。
- 时钟黑条位于顶部中央偏左，Desktop Top 的黑条在右侧，间隔 20 像素。窗口和黑条均遵循只在空工作区显示的规则。
- 点击终端后接收键盘、鼠标和滚轮输入；平时不抢键盘焦点。btop 内按 `q` 退出后，点击底部提示重新启动。
- 位于壁纸上方，不参与普通窗口平铺，不预留桌面空间；独立软件渲染进程，与 Omarchy 和 Wave 分开。
- 每个空工作区显示器独立运行 btop，最小化期间继续运行；工作区被应用窗口占用后停止对应进程，禁用插件后停止所有渲染与终端进程。
- 使用私有的临时 btop 配置和主题，读取现有配置作为基础，启用透明背景、每秒刷新和鼠标输入；不改写用户的 btop 配置。

## 安全性

终端把 btop 的 PTY 输出送入 libvterm。libvterm 0.3.3 及更早版本在每遇到一个 `;` 或 `:` 时递增 CSI 参数下标而不检查数组边界（`parser.c:229-235`），而 btop 树状视图渲染进程 argv 时不转义控制字符，因此本机其他用户可以注入超过 16 个参数的序列，在本进程内越过 libvterm 的参数数组写入。

为此插件在 PTY 边界自行限制参数分隔符的数量（`native/CsiFilter.cpp`）。过滤器按 UTF-8 模式复刻 libvterm 的 ESC/CSI 解析：待处理的 ESC 会跨 BEL、其他 C0 控制字符、DEL 和高位字节保留，并跟踪转义中间字节，使随后的 `[` 结束转义而不是开启 CSI。只有会开启第 17 个参数的分隔符被丢弃，正文、UTF-8 和 DCS/OSC 负载均逐字节原样通过。该防护不依赖系统 btop 或 libvterm 的版本，由 `tests/csi_filter.test.cpp` 覆盖，并可接入真实 libvterm 解析器验证。

## 安装

需要 Omarchy / Quickshell、使用 Lua 配置的 Hyprland、btop、Qt 6.5+（Quick/Qml）、libvterm 0.3+、CMake、pkg-config 和 C++ 编译器。
在 Arch Linux 中对应 `quickshell btop qt6-declarative libvterm cmake pkgconf gcc`。

```sh
git clone https://github.com/manateelazycat/omarchy-desktop-top.git
cd omarchy-desktop-top
bash install.sh
```

安装器编译本地终端模块，将完整插件安装到 `~/.config/omarchy/plugins/io.github.manateelazycat.desktop-top/`，添加仅针对该插件的动画规则，重新加载 Hyprland 并检查配置错误，然后启用插件。
配置与旧插件备份到 `~/.local/state/omarchy/desktop-top/backups/`。支持 `XDG_CONFIG_HOME` 和 `XDG_STATE_HOME`。

更新代码并保持当前启用状态：

```sh
bash install.sh --no-enable
```

Qt / libvterm 更新后，可以重新运行安装器编译本机模块。

### 插件商店安装后的设置

商店的通用安装器只克隆仓库，不编译终端模块，也不添加 Hyprland 图层规则。添加插件后，需要明确执行：

```sh
bash "${XDG_CONFIG_HOME:-$HOME/.config}/omarchy/plugins/io.github.manateelazycat.desktop-top/install.sh" --no-enable
omarchy plugin enable io.github.manateelazycat.desktop-top
```

设置会保留已安装目录中的源码和 Git 元数据，因此仍可使用 `omarchy plugin update`。更新后再次执行设置命令编译本地模块。设置脚本在添加专属、带文件存在检查的 Hyprland 引用前备份用户配置，不需要 sudo。

### 卸载

```sh
omarchy plugin remove io.github.manateelazycat.desktop-top
```

此命令在确认后停用插件并删除安装目录。插件移除后，`~/.config/hypr/hyprland.lua` 中的引用自动失效。如需一并清理，删除从 `-- >>> omarchy-desktop-top >>>` 到 `-- <<< omarchy-desktop-top <<<` 的整个区块，再执行 `hyprctl reload` 和 `hyprctl configerrors`。状态目录中的配置备份保留。

## 配置

在 `~/.config/omarchy/shell.json` 的现有插件条目中调整：

```json
{
  "id": "io.github.manateelazycat.desktop-top",
  "positionX": 0.64,
  "positionY": 0.52,
  "width": 1000,
  "height": 650,
  "fontSize": 13,
  "opacity": 1,
  "boxes": "cpu mem net proc"
}
```

`positionX` / `positionY` 范围为 0–1，分别对应可用移动范围的左到右、上到下。`width` / `height` 使用逻辑像素；内容区左右各 26 像素，顶部 48 像素，底部 50 像素。
`fontSize` 范围为 8–32，`opacity` 范围为 0.2–1；默认背景透明度与时钟一致，为 0.76。
`boxes` 支持 `cpu mem net proc` 的组合，缩放下限按这些启动面板的 btop 尺寸规则计算。终端中临时切换面板不保存到插件配置。
若显示器连最小窗口加 24 像素四周边距都无法容纳，该显示器不显示插件；可降低字号或减少启动面板。

```sh
omarchy-shell desktop-top status
omarchy-shell desktop-top setPosition 0.64 0.52
omarchy-shell desktop-top resetPosition
omarchy-shell desktop-top setSize 1000 650
omarchy-shell desktop-top resetSize
omarchy plugin disable io.github.manateelazycat.desktop-top
omarchy plugin enable io.github.manateelazycat.desktop-top
```

`status` 返回渲染进程、每个显示器的可见状态、窗口大小、终端行列数和 btop PID。禁用插件会移除内联配置条目，安装备份保留原设置。

## 验证

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
omarchy plugin validate .
g++ -std=c++17 -Wall -Wextra -Werror -o /tmp/csi_filter.test tests/csi_filter.test.cpp native/CsiFilter.cpp && /tmp/csi_filter.test
g++ -std=c++17 -Wall -Wextra -Werror -DCSI_FILTER_WITH_VTERM -o /tmp/csi_filter-vterm.test tests/csi_filter.test.cpp native/CsiFilter.cpp $(pkg-config --cflags --libs vterm) && /tmp/csi_filter-vterm.test
node --test tests/geometry.test.cjs
bash tests/run-runtime.sh
python3 tests/bridge.py
python3 tests/install.py
```

运行时检查在独立进程中使用合成 Qt 鼠标与键盘事件，不移动系统指针、不写用户设置。覆盖真实 btop 输出、拖动稳定性、八向缩放、最小 PTY、键盘输入、主题更新、尺寸恢复、空工作区显示和进程退出。安装器测试使用临时目录，验证反复执行商店设置会保留源码和 Git 元数据。

## 许可证

[GPL-3.0-only](LICENSE)。外框、桌面占用检测及主进程通信改编自 Omarchy Desktop Clock，Copyright (C) 2026 ManateeLazyCat。
