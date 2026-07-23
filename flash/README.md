# TuyaOpen Bridge 烧录工具

> ESP32-S3 TuyaOpen Bridge 项目的 Windows 烧录工具
> 一键烧录固件 + 自动监控串口日志 + 配网二维码捕获

---

## 功能特性

- **自动扫描串口**：自动识别 CH340/CP2102 等 USB 转串口设备
- **可选烧录频率**：115200 / 460800 / 921600 / 1500000 bps
- **可选擦除 flash**：烧录前可选擦除整片 flash（清除 NVS 残留）
- **烧录后自动监控**：烧录完成自动打开串口监控
- **自动捕获二维码**：实时解析串口日志中的 `SetupQRCode: [MT:xxx]`（Matter 兼容）
- **二维码自动保存**：检测到二维码后自动保存 PNG 到 `qr_codes` 文件夹
- **二维码打印**：调用 Windows 打印机直接打印（A4 居中）
- **二维码手动保存**：另存为 PNG 图片到指定位置
- **设置持久化**：记住上次使用的串口、波特率、固件路径
- **不依赖编译环境**：直接调用 esptool 模块烧录，无需配置 TuyaOpen/ESP-IDF 环境
- **框架无关**：兼容 TuyaOpen SDK 和 ESP-IDF 构建的固件

---

## 环境要求

### 运行 EXE 版本（推荐，零依赖）

- Windows 10/11（64 位）
- USB 转串口驱动已安装（CH340 / CP2102）
- esptool 已安装（`pip install esptool`）或使用包含 esptool 的打包版本

### 运行 Python 源码版本

- Python 3.8+
- 依赖：`pip install -r requirements.txt`

---

## 快速开始

### 方式 1：使用打包好的 EXE

1. 双击运行 `TuyaOpenBridgeFlashTool.exe`
2. 选择串口号（自动扫描）
3. 选择波特率（默认 921600）
4. 选择固件目录（包含 `flasher_args.json` 的 `.build` 目录）
5. 勾选"擦除 flash"（首次烧录或配网异常时建议勾选）
6. 点击"擦除 flash 并烧录"或"仅烧录"
7. 烧录完成后自动进入串口监控

### 方式 2：从源码运行

```powershell
cd e:\AI\tuya-broker\flash
pip install -r requirements.txt
python flash_tool.py
```

---

## 自行打包 EXE

```powershell
cd e:\AI\tuya-broker\flash
.\build_exe.bat
```

打包完成后，EXE 位于 `dist\TuyaOpenBridgeFlashTool.exe`。

---

## 界面说明

```
┌─────────────────────────────────────────────────────────┐
│            TuyaOpen Bridge 烧录工具 v1.2                  │
├──────────────────────┬──────────────────────────────────┤
│  烧录配置            │     配网二维码                    │
│                      │                                  │
│  串口号: [COM5 ▼] 🔄 │   ┌──────────────────────┐       │
│  波特率: [921600 ▼]  │   │                      │       │
│  固件目录: [.....] 📁│   │      [二维码图片]     │       │
│  ☐ 擦除 flash        │   │                      │       │
│  ☑ 烧录后自动监控    │   └──────────────────────┘       │
│                      │   MT 码: [MT:Y.K9042C00KA0648G00]│
│  [擦除并烧录]        │   手动码: [34970112332]           │
│  [仅烧录]            │   [打印二维码] [保存二维码]       │
│  [停止监控]          │                                  │
├──────────────────────┴──────────────────────────────────┤
│  日志输出：                                              │
│  [12:30:45] 开始烧录 | 端口=COM5 | 波特率=921600        │
│  [12:30:46] 执行烧录: esptool --chip esp32s3 ...        │
│  [12:31:20] 烧录完成                                    │
│  [12:31:21] 串口 COM5 已打开                            │
└─────────────────────────────────────────────────────────┘
```

> **说明**：本项目使用涂鸦 BLE+AP 配网，不使用 Matter 二维码。
> 二维码区域保留用于 Matter 兼容场景，TuyaOpen 模式下该区域不显示内容。

---

## 配置文件

工具运行后会在 EXE/脚本同目录生成 `settings.json`，保存以下设置：

```json
{
  "port": "COM5",
  "baudrate": 921600,
  "firmware_dir": "e:\\AI\\tuya-broker\\TuyaOpen\\apps\\tuya_cloud\\window_broker\\.build",
  "erase": false,
  "auto_monitor": true
}
```

- `port`：保存纯串口名（如 `COM5`），更换 USB 口后自动匹配
- `firmware_dir`：首次使用需手动选择固件目录（`.build` 目录）

---

## 二维码自动保存

检测到设备输出 `SetupQRCode` 日志后，工具会自动将二维码 PNG 保存到 EXE/脚本同目录的 `qr_codes` 文件夹中。

- 文件名格式：`时间戳_MT码.png`（如 `20260701_123121_MT_Y.K9042C00KA0648G00.png`）
- 同一 MT 码只保存一次，避免重复文件堆积
- 设备重新配网后（MT 码变化）会自动保存新文件

> **注意**：TuyaOpen 项目使用 BLE+AP 配网，不输出 `SetupQRCode` 日志，此功能仅对 Matter 固件有效。

---

## 常见问题

### Q1: 烧录失败，提示"esptool 未安装"

**原因**：esptool 模块未安装。
**解决**：运行 `pip install esptool` 安装。

### Q2: 串口扫描不到设备

**原因**：
- USB 转串口驱动未安装
- 设备未连接或被其他程序占用

**解决**：
1. 安装 CH340 驱动（ESP32 开发板常用）
2. 关闭其他串口监控程序（如 PuTTY、Arduino IDE）
3. 点击"刷新"按钮重新扫描

### Q3: 烧录中关闭窗口的警告

**原因**：esptool 是同步阻塞调用，无法安全中断。
**说明**：烧录过程中关闭窗口会弹出警告，强制关闭可能导致设备固件不完整。请等待烧录完成后再关闭。

### Q4: 固件目录选择后提示"未找到 flasher_args.json"

**原因**：所选目录不是构建输出目录，或尚未编译。
**解决**：
1. 先执行 `tos.py build` 编译固件
2. 选择 `.build` 目录（TuyaOpen）或 `build` 目录（ESP-IDF）

### Q5: 打印功能不可用

**原因**：`pywin32` 未安装或打包时丢失。
**解决**：
- 源码运行：`pip install pywin32`
- EXE 运行：重新打包，确认 spec 文件中包含 `--hidden-import "win32print"`

### Q6: 打包后 EXE 体积过大

**原因**：PIL 和 qrcode 库较大。
**解决**：可改用 `--onedir` 替代 `--onefile`（修改 spec 文件），体积更小但需要分发整个目录。

---

## 技术实现

| 模块 | 技术栈 |
|------|--------|
| GUI | tkinter（Python 内置） |
| 串口 | pyserial |
| 二维码 | qrcode + Pillow |
| 打印 | win32print + PIL.ImageWin.Dib |
| 烧录 | 直接调用 esptool Python 模块（不依赖编译环境） |
| 打包 | PyInstaller |
| 设置持久化 | json |

---

## 文件结构

```
flash/
├── flash_tool.py                    # 主程序源码
├── MatterBridgeFlashTool.spec       # PyInstaller 打包配置
├── build_exe.bat                    # EXE 打包脚本
├── requirements.txt                 # Python 依赖
├── README.md                        # 本文档
├── settings.json                    # 运行时自动生成
└── qr_codes/                        # 二维码自动保存目录（运行时自动创建）
    └── 20260701_123121_MT_xxx.png
```

---

## 注意事项

1. **固件目录**：需选择构建输出目录（包含 `flasher_args.json`），TuyaOpen 项目为 `.build` 目录，首次使用需手动选择
2. **配置文件**：`settings.json` 和 `qr_codes` 文件夹自动创建在 EXE/脚本同目录
3. **擦除 flash**：会清除所有 NVS 数据（包括 WiFi 凭证和配网信息），需重新配网
4. **串口占用**：烧录和监控使用同一串口，监控时会自动释放给烧录使用
5. **烧录中断**：esptool 无法安全中断，烧录过程中请勿关闭程序或断开设备
6. **框架兼容**：本工具基于 esptool + flasher_args.json 烧录，兼容 TuyaOpen SDK 和 ESP-IDF 构建的固件
