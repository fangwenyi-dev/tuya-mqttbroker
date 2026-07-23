# -*- coding: utf-8 -*-
"""
TuyaOpen Bridge 烧录工具 v1.2
===============================
ESP32-S3 TuyaOpen Bridge 项目的烧录与监控工具。

功能：
1. 自动扫描串口
2. 调用 esptool 进行烧录（可选擦除 flash）
3. 烧录后自动监控串口日志
4. 解析日志中的配网二维码（MT: 开头字符串，Matter 兼容）
5. 生成、显示、打印、保存二维码
6. 配置持久化

说明：
    本工具基于 esptool + flasher_args.json 烧录，与编译框架无关，
    兼容 TuyaOpen SDK 和 ESP-IDF 构建的固件。

依赖：
    pyserial>=3.5
    qrcode[pil]>=7.4
    Pillow>=9.0
    esptool>=4.7
    pywin32>=305  ; Windows API（打印功能，可选）
"""

import os
import sys
import re
import io
import json
import time
import threading
import subprocess
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
from datetime import datetime
from contextlib import redirect_stdout, redirect_stderr

# 串口相关
try:
    import serial
    from serial.tools import list_ports
    SERIAL_AVAILABLE = True
except ImportError:
    SERIAL_AVAILABLE = False
    list_ports = None

# 二维码相关
try:
    import qrcode
    from PIL import Image, ImageTk, ImageWin
    QRCODE_AVAILABLE = True
except ImportError:
    QRCODE_AVAILABLE = False
    qrcode = None
    ImageTk = None
    ImageWin = None

# Windows 打印 API（可选）
try:
    import win32print
    import win32ui
    WIN32PRINT_AVAILABLE = True
except ImportError:
    WIN32PRINT_AVAILABLE = False
    win32print = None
    win32ui = None

try:
    from tkinter.scrolledtext import ScrolledText
except ImportError:
    ScrolledText = None


# ====================================================================
# 全局常量
# ====================================================================
APP_TITLE = "TuyaOpen Bridge 烧录工具"
APP_VERSION = "v1.2"
WINDOW_SIZE = "900x700"

# 固件清单文件名（TuyaOpen/ESP-IDF build 生成）
FLASH_ARGS_FILE = "flasher_args.json"
# 默认芯片类型（当 flasher_args.json 未指定时使用）
DEFAULT_CHIP = "esp32s3"

# 串口与烧录
DEFAULT_BAUDRATE = 921600
MONITOR_BAUDRATE = 115200
BAUDRATE_OPTIONS = [115200, 460800, 921600, 1500000]

# 二维码
QR_DISPLAY_SIZE = 300
QR_BORDER = 10
# 二维码自动保存文件夹名
QR_SAVE_DIR_NAME = "qr_codes"
# 日志区最大行数（超出后自动删除旧行，防止内存无限增长）
LOG_MAX_LINES = 2000


def _get_app_dir():
    """获取应用程序所在目录（兼容 PyInstaller 打包和源码运行）。"""
    if getattr(sys, 'frozen', False):
        # PyInstaller 打包后，sys.executable 指向 EXE 路径
        return os.path.dirname(sys.executable)
    else:
        # 源码运行，__file__ 指向脚本路径
        return os.path.dirname(os.path.abspath(__file__))


# 配置文件与应用目录（动态获取，不硬编码）
APP_DIR = _get_app_dir()
SETTINGS_FILE = os.path.join(APP_DIR, "settings.json")
# 二维码自动保存目录
QR_SAVE_DIR = os.path.join(APP_DIR, QR_SAVE_DIR_NAME)

# 正则：匹配 SetupQRCode: [MT:xxxx]
QR_REGEX = re.compile(r"SetupQRCode:\s*\[(MT:[^\]]+)\]")
# 正则：匹配 Manual pairing code
MANUAL_CODE_REGEX = re.compile(r"Manual pairing code:\s*\[(\d+)\]")

# 日志颜色标签
TAG_INFO = "info"
TAG_HIGHLIGHT = "highlight"
TAG_ERROR = "error"
TAG_SUCCESS = "success"
TAG_SERIAL = "serial"

# ANSI 颜色码正则（esptool 输出可能包含）
ANSI_ESCAPE = re.compile(r'\033\[[0-9;]*m')


class _EsptoolStream(io.TextIOBase):
    """重定向 esptool 的 stdout/stderr 输出到日志区。

    处理 esptool 输出特点：
    - 进度条用 \\r 回车符刷新（Writing at 0x... (50 %)\r）
    - 普通行用 \\n 结尾
    - 可能包含 ANSI 颜色码 \\033[0;32m
    """

    def __init__(self, log_func):
        # log_func: 签名 (message, tag) -> None
        self._log = log_func
        self._buffer = ""

    def write(self, text):
        if not text:
            return 0
        # 清理 ANSI 颜色码
        text = ANSI_ESCAPE.sub('', text)
        self._buffer += text
        # 同时按 \r 和 \n 分割（\r 表示进度条刷新，作为新行处理）
        # 这样每次 \r 后的内容会作为新的一行输出，模拟进度条刷新效果
        parts = re.split(r'[\r\n]', self._buffer)
        # 最后一段可能不完整，保留在 buffer
        self._buffer = parts.pop()
        for line in parts:
            line = line.rstrip()
            if line:
                self._log(line, TAG_SERIAL)
        return len(text)

    def flush(self):
        # 输出 buffer 中剩余内容
        if self._buffer:
            line = ANSI_ESCAPE.sub('', self._buffer).rstrip()
            if line:
                self._log(line, TAG_SERIAL)
            self._buffer = ""


class FlashToolApp:
    """Matter Bridge 烧录工具主应用类。"""

    # ================================================================
    # 初始化
    # ================================================================
    def __init__(self):
        # 主窗口
        self.root = tk.Tk()
        self.root.title(f"{APP_TITLE} {APP_VERSION}")
        self.root.geometry(WINDOW_SIZE)
        self.root.minsize(800, 600)

        # 关闭窗口时的清理
        self.root.protocol("WM_DELETE_WINDOW", self._on_closing)

        # ---------- 状态变量 ----------
        self.port_var = tk.StringVar()
        self.baud_var = tk.IntVar(value=DEFAULT_BAUDRATE)
        self.firmware_dir_var = tk.StringVar(value="")
        self.erase_var = tk.BooleanVar(value=False)
        self.auto_monitor_var = tk.BooleanVar(value=True)
        self.mt_code_var = tk.StringVar(value="等待设备输出二维码...")
        self.manual_code_var = tk.StringVar(value="")

        # 线程与运行控制
        self._flash_thread = None
        self._monitor_thread = None
        self._stop_flash_event = threading.Event()
        self._stop_monitor_event = threading.Event()
        self._serial_port = None           # 当前监控使用的串口对象
        self._is_flashing = False          # 烧录进行中标志
        self._is_monitoring = False        # 监控进行中标志
        self._current_qr_image = None      # 保持 PhotoImage 引用避免被 GC
        self._current_pil_image = None     # 当前 PIL 图像（用于打印/保存）
        self._last_saved_mt_code = None    # 上次自动保存的 MT 码（避免重复保存）
        self._saved_port = None            # 从配置文件加载的串口名，扫描后匹配

        # ---------- 加载设置 ----------
        self._load_settings()

        # ---------- 构建 UI ----------
        self._build_ui()

        # ---------- 扫描串口 ----------
        self._scan_serial_ports()

    # ================================================================
    # UI 构建
    # ================================================================
    def _build_ui(self):
        """构建整个界面。"""
        # 顶部标题区
        self._build_top_frame()
        # 主区域：左右两栏
        main_frame = ttk.Frame(self.root, padding=5)
        main_frame.grid(row=1, column=0, sticky="nsew")
        self.root.rowconfigure(1, weight=1)
        self.root.columnconfigure(0, weight=1)

        main_frame.columnconfigure(0, weight=1, uniform="lr")
        main_frame.columnconfigure(1, weight=1, uniform="lr")
        main_frame.rowconfigure(0, weight=1)

        self._build_left_config_frame(main_frame)
        self._build_right_qrcode_frame(main_frame)
        # 底部日志区
        self._build_bottom_log_frame()

    def _build_top_frame(self):
        """构建顶部标题区。"""
        top_frame = ttk.Frame(self.root, padding=10)
        top_frame.grid(row=0, column=0, sticky="ew")
        title_label = ttk.Label(
            top_frame,
            text=f"{APP_TITLE} {APP_VERSION}",
            font=("微软雅黑", 16, "bold"),
            anchor="center"
        )
        title_label.pack(fill="x")
        subtitle = ttk.Label(
            top_frame,
            text="ESP32-S3 TuyaOpen Bridge | TuyaOpen SDK (ESP-IDF v5.4)",
            font=("微软雅黑", 9),
            anchor="center",
            foreground="gray"
        )
        subtitle.pack(fill="x")

    def _build_left_config_frame(self, parent):
        """构建左侧配置区。"""
        left_frame = ttk.LabelFrame(parent, text="配置与烧录", padding=10)
        left_frame.grid(row=0, column=0, sticky="nsew", padx=(0, 5))
        left_frame.columnconfigure(1, weight=1)

        row = 0

        # 串口号
        ttk.Label(left_frame, text="串口号:").grid(row=row, column=0, sticky="w", pady=5)
        port_frame = ttk.Frame(left_frame)
        port_frame.grid(row=row, column=1, sticky="ew", pady=5)
        port_frame.columnconfigure(0, weight=1)
        self.port_combo = ttk.Combobox(port_frame, textvariable=self.port_var, state="readonly")
        self.port_combo.grid(row=0, column=0, sticky="ew")
        self.refresh_port_btn = ttk.Button(port_frame, text="刷新", width=6, command=self._on_refresh_ports)
        self.refresh_port_btn.grid(
            row=0, column=1, padx=(5, 0)
        )
        row += 1

        # 波特率
        ttk.Label(left_frame, text="烧录波特率:").grid(row=row, column=0, sticky="w", pady=5)
        self.baud_combo = ttk.Combobox(
            left_frame,
            textvariable=self.baud_var,
            values=[str(b) for b in BAUDRATE_OPTIONS],
            state="readonly",
            width=10
        )
        self.baud_combo.grid(row=row, column=1, sticky="w", pady=5)
        row += 1

        # 固件目录（需含 flasher_args.json）
        ttk.Label(left_frame, text="固件目录:").grid(row=row, column=0, sticky="w", pady=5)
        dir_frame = ttk.Frame(left_frame)
        dir_frame.grid(row=row, column=1, sticky="ew", pady=5)
        dir_frame.columnconfigure(0, weight=1)
        self.firmware_dir_entry = ttk.Entry(dir_frame, textvariable=self.firmware_dir_var)
        self.firmware_dir_entry.grid(row=0, column=0, sticky="ew")
        self.browse_dir_btn = ttk.Button(dir_frame, text="浏览", width=6, command=self._on_browse_dir)
        self.browse_dir_btn.grid(
            row=0, column=1, padx=(5, 0)
        )
        row += 1

        # 选项复选框
        self.erase_checkbtn = ttk.Checkbutton(
            left_frame, text="擦除 flash（烧录前）", variable=self.erase_var
        )
        self.erase_checkbtn.grid(row=row, column=0, columnspan=2, sticky="w", pady=5)
        row += 1

        self.auto_monitor_checkbtn = ttk.Checkbutton(
            left_frame, text="烧录后自动监控串口", variable=self.auto_monitor_var
        )
        self.auto_monitor_checkbtn.grid(row=row, column=0, columnspan=2, sticky="w", pady=5)
        row += 1

        # 分隔线
        ttk.Separator(left_frame, orient="horizontal").grid(
            row=row, column=0, columnspan=2, sticky="ew", pady=10
        )
        row += 1

        # 操作按钮
        self.erase_flash_btn = ttk.Button(
            left_frame, text="擦除 flash 并烧录", command=self._on_erase_and_flash
        )
        self.erase_flash_btn.grid(row=row, column=0, columnspan=2, sticky="ew", pady=3)
        row += 1

        self.flash_btn = ttk.Button(
            left_frame, text="仅烧录", command=self._on_flash_only
        )
        self.flash_btn.grid(row=row, column=0, columnspan=2, sticky="ew", pady=3)
        row += 1

        self.stop_monitor_btn = ttk.Button(
            left_frame, text="停止监控", command=self._on_stop_monitor, state=tk.DISABLED
        )
        self.stop_monitor_btn.grid(row=row, column=0, columnspan=2, sticky="ew", pady=3)
        row += 1

        # 状态标签
        self.status_label = ttk.Label(left_frame, text="状态: 就绪", foreground="blue")
        self.status_label.grid(row=row, column=0, columnspan=2, sticky="w", pady=(10, 0))

    def _build_right_qrcode_frame(self, parent):
        """构建右侧二维码区。"""
        right_frame = ttk.LabelFrame(parent, text="配网二维码", padding=10)
        right_frame.grid(row=0, column=1, sticky="nsew", padx=(5, 0))
        right_frame.columnconfigure(0, weight=1)

        # 二维码显示区（用 Frame 固定像素尺寸，避免 Label width/height 单位为字符）
        qr_frame = tk.Frame(
            right_frame,
            width=QR_DISPLAY_SIZE,
            height=QR_DISPLAY_SIZE,
            bg="white",
            relief="solid",
            bd=1
        )
        qr_frame.pack(pady=10)
        qr_frame.pack_propagate(False)  # 禁止子控件影响 Frame 尺寸
        self.qr_label = tk.Label(
            qr_frame,
            text="等待设备输出\n配网二维码...",
            bg="white"
        )
        self.qr_label.pack(expand=True, fill="both")

        # MT 码文本
        ttk.Label(right_frame, text="MT 码:").pack(anchor="w", pady=(5, 0))
        mt_entry = ttk.Entry(right_frame, textvariable=self.mt_code_var, state="readonly")
        mt_entry.pack(fill="x", pady=2)

        # 手动配对码
        ttk.Label(right_frame, text="手动配对码:").pack(anchor="w", pady=(5, 0))
        manual_entry = ttk.Entry(right_frame, textvariable=self.manual_code_var, state="readonly")
        manual_entry.pack(fill="x", pady=2)

        # 二维码操作按钮
        btn_frame = ttk.Frame(right_frame)
        btn_frame.pack(fill="x", pady=10)
        btn_frame.columnconfigure(0, weight=1)
        btn_frame.columnconfigure(1, weight=1)
        ttk.Button(btn_frame, text="打印二维码", command=self._on_print_qrcode).grid(
            row=0, column=0, sticky="ew", padx=2
        )
        ttk.Button(btn_frame, text="保存二维码", command=self._on_save_qrcode).grid(
            row=0, column=1, sticky="ew", padx=2
        )

    def _build_bottom_log_frame(self):
        """构建底部日志区。"""
        log_frame = ttk.LabelFrame(self.root, text="日志", padding=5)
        log_frame.grid(row=2, column=0, sticky="nsew", padx=10, pady=(0, 10))
        log_frame.rowconfigure(0, weight=1)
        log_frame.columnconfigure(0, weight=1)
        self.root.rowconfigure(2, weight=1, minsize=200)

        if ScrolledText is not None:
            self.log_text = ScrolledText(
                log_frame, wrap=tk.WORD, height=10, font=("Consolas", 9)
            )
        else:
            # 兼容回退
            self.log_text = tk.Text(log_frame, wrap=tk.WORD, height=10, font=("Consolas", 9))
        self.log_text.grid(row=0, column=0, sticky="nsew")

        # 配置日志颜色标签
        self.log_text.tag_configure(TAG_INFO, foreground="black")
        self.log_text.tag_configure(TAG_HIGHLIGHT, foreground="blue", background="#FFFFCC",
                                    font=("Consolas", 9, "bold"))
        self.log_text.tag_configure(TAG_ERROR, foreground="red")
        self.log_text.tag_configure(TAG_SUCCESS, foreground="green")
        self.log_text.tag_configure(TAG_SERIAL, foreground="#444444")
        self.log_text.configure(state=tk.DISABLED)

    # ================================================================
    # 串口扫描
    # ================================================================
    def _scan_serial_ports(self):
        """扫描所有可用串口并填充下拉框。"""
        if not SERIAL_AVAILABLE:
            self._log("pyserial 未安装，无法扫描串口", TAG_ERROR)
            return

        ports = list(list_ports.comports())
        if not ports:
            self.port_combo["values"] = []
            self.port_var.set("")
            self._log("未发现可用串口", TAG_ERROR)
            return

        # 格式：COM5 - USB-SERIAL CH340 (COM5)
        port_items = []
        for p in ports:
            if p.description:
                display = f"{p.device} - {p.description}"
            else:
                display = p.device
            port_items.append(display)

        self.port_combo["values"] = port_items

        # 优先匹配从配置文件加载的端口名；否则保留之前的选择；否则选第一个
        matched = False
        if self._saved_port:
            for item in port_items:
                if item.startswith(self._saved_port + " - ") or item == self._saved_port:
                    self.port_combo.set(item)
                    matched = True
                    break
            self._saved_port = None  # 匹配完毕，清除

        if not matched:
            current = self.port_var.get()
            if current and current in port_items:
                self.port_combo.set(current)
            else:
                self.port_combo.current(0)
                self.port_var.set(port_items[0])

        self._log(f"扫描到 {len(ports)} 个串口: {', '.join(p.device for p in ports)}")

    def _on_refresh_ports(self):
        """刷新按钮回调。"""
        self._log("刷新串口列表...")
        self._scan_serial_ports()

    def _get_selected_port_name(self):
        """从下拉框文本中提取纯串口名（如 COM5）。"""
        text = self.port_var.get()
        if not text:
            return ""
        # 取 " - " 之前的部分
        if " - " in text:
            return text.split(" - ")[0].strip()
        return text.strip()

    # ================================================================
    # 目录浏览
    # ================================================================
    def _on_browse_dir(self):
        """浏览固件目录。"""
        current = self.firmware_dir_var.get()
        initial = current if os.path.isdir(current) else APP_DIR
        chosen = filedialog.askdirectory(title="选择固件目录", initialdir=initial)
        if chosen:
            self.firmware_dir_var.set(chosen)
            self._log(f"固件目录已选择: {chosen}")

    # ================================================================
    # 烧录功能
    # ================================================================
    def _on_erase_and_flash(self):
        """擦除 flash 并烧录按钮回调。"""
        self.erase_var.set(True)
        self._start_flash(erase=True)

    def _on_flash_only(self):
        """仅烧录按钮回调。"""
        self.erase_var.set(False)
        self._start_flash(erase=False)

    def _start_flash(self, erase=False):
        """启动烧录流程。"""
        if self._is_flashing:
            messagebox.showwarning("提示", "烧录正在进行中，请等待完成。")
            return
        if self._is_monitoring:
            self._stop_monitor(wait_thread=True)

        port = self._get_selected_port_name()
        if not port:
            messagebox.showerror("错误", "请先选择串口！")
            return

        # 校验固件目录存在
        firmware_dir = self.firmware_dir_var.get().strip()
        if not firmware_dir or not os.path.isdir(firmware_dir):
            messagebox.showerror("错误", f"固件目录不存在:\n{firmware_dir}")
            return

        # 校验 flasher_args.json 存在
        args_file = os.path.join(firmware_dir, FLASH_ARGS_FILE)
        if not os.path.isfile(args_file):
            messagebox.showerror(
                "错误",
                f"未找到固件清单文件:\n{args_file}\n\n请先运行 idf.py build 生成固件。"
            )
            return

        try:
            baudrate = int(self.baud_var.get())
        except (ValueError, TypeError):
            baudrate = DEFAULT_BAUDRATE

        # 重置停止事件
        self._stop_flash_event.clear()
        self._is_flashing = True
        self._set_buttons_state(flashing=True)

        self._log("=" * 60, TAG_INFO)
        self._log(f"开始烧录 | 端口={port} | 波特率={baudrate} | 擦除={erase}",
                  TAG_INFO)
        self._log("=" * 60, TAG_INFO)

        self._flash_thread = threading.Thread(
            target=self._flash_thread_func,
            args=(port, baudrate, erase, firmware_dir),
            daemon=True
        )
        self._flash_thread.start()

    def _parse_flasher_args(self, args_file):
        """解析 flasher_args.json，返回烧录参数字典。

        返回结构：
        {
            "chip": str,
            "before": str,
            "after": str,
            "write_flash_args": [str, ...],  # 完整的 write-flash 参数（已转连字符格式）
            "flash_files": [(offset, file_path), ...]
        }

        注：参数名与参数值统一转换为 esptool 5.x 新格式（连字符），
        消除 "Deprecated" 警告：
        - --flash_mode  -> --flash-mode
        - --flash_size  -> --flash-size
        - --flash_freq  -> --flash-freq
        - default_reset -> default-reset
        - hard_reset    -> hard-reset
        """
        with open(args_file, "r", encoding="utf-8") as f:
            data = json.load(f)

        # 芯片类型与复位方式（值转换为新格式）
        extra = data.get("extra_esptool_args", {})
        chip = extra.get("chip", DEFAULT_CHIP)
        before = extra.get("before", "default-reset").replace("_", "-")
        after = extra.get("after", "hard-reset").replace("_", "-")

        # Flash 参数：直接使用 write_flash_args 完整列表，将下划线参数名转连字符
        # 这样不会遗漏 --encrypt、--compress 等参数
        write_flash_args = []
        for arg in data.get("write_flash_args", []):
            if arg.startswith("--") and "_" in arg:
                write_flash_args.append(arg.replace("_", "-"))
            else:
                write_flash_args.append(arg)

        # 固件文件列表
        flash_files = []
        flash_files_dict = data.get("flash_files", {})
        for offset, fname in flash_files_dict.items():
            flash_files.append((offset, fname))

        # 按偏移地址排序，方便日志展示
        flash_files.sort(key=lambda x: int(x[0], 16))

        return {
            "chip": chip,
            "before": before,
            "after": after,
            "write_flash_args": write_flash_args,
            "flash_files": flash_files,
        }

    def _flash_thread_func(self, port, baudrate, erase, firmware_dir):
        """烧录线程函数：进程内调用 esptool.main() 烧录，不依赖 ESP-IDF 环境。"""
        try:
            args_file = os.path.join(firmware_dir, FLASH_ARGS_FILE)

            # 1. 解析固件清单
            try:
                params = self._parse_flasher_args(args_file)
            except (OSError, json.JSONDecodeError) as e:
                self._log(f"解析固件清单失败: {e}", TAG_ERROR)
                self._update_status("烧录失败", "red")
                return

            # 2. 校验所有固件文件都存在
            missing = []
            for offset, fname in params["flash_files"]:
                fpath = os.path.join(firmware_dir, fname)
                if not os.path.isfile(fpath):
                    missing.append(fpath)
            if missing:
                self._log("以下固件文件缺失:", TAG_ERROR)
                for m in missing:
                    self._log(f"  {m}", TAG_ERROR)
                self._update_status("烧录失败", "red")
                return

            # 3. 预览烧录文件清单
            self._log(f"芯片: {params['chip']}", TAG_INFO)
            self._log(f"write-flash 参数: {' '.join(params['write_flash_args'])}", TAG_INFO)
            self._log(f"将烧录 {len(params['flash_files'])} 个文件:", TAG_HIGHLIGHT)
            for idx, (offset, fname) in enumerate(params["flash_files"], 1):
                fpath = os.path.join(firmware_dir, fname)
                size = os.path.getsize(fpath)
                self._log(f"  {idx}) {fname} @ {offset} ({size} bytes)", TAG_INFO)

            # 4. 构造 esptool 参数列表（传给 esptool.main，不含 sys.executable）
            #    固件文件路径用绝对路径，避免依赖 cwd
            common_args = [
                "--chip", params["chip"],
                "-p", port,
                "-b", str(baudrate),
                f"--before={params['before']}",
                f"--after={params['after']}",
            ]

            # 5. 若需擦除，先执行 erase-flash
            if erase:
                erase_args = common_args + ["erase-flash"]
                self._log("执行擦除: esptool " + " ".join(erase_args), TAG_INFO)
                erase_ok = self._run_esptool(erase_args)
                if not erase_ok or self._stop_flash_event.is_set():
                    if not self._stop_flash_event.is_set():
                        self._log("擦除失败，终止烧录", TAG_ERROR)
                        self._update_status("烧录失败", "red")
                    return
                self._log("擦除完成", TAG_SUCCESS)

            # 6. 构造 write-flash 参数（使用 flasher_args.json 中的完整参数 + 绝对路径固件文件）
            write_args = common_args + ["write-flash"] + params["write_flash_args"]
            for offset, fname in params["flash_files"]:
                abs_path = os.path.join(firmware_dir, fname)
                write_args.extend([offset, abs_path])

            self._log("执行烧录: esptool " + " ".join(write_args), TAG_INFO)

            # 7. 执行烧录
            flash_ok = self._run_esptool(write_args)

            if self._stop_flash_event.is_set():
                self._update_status("已中断", "red")
                return

            if flash_ok:
                self._log("烧录完成！", TAG_SUCCESS)
                self._update_status("烧录完成", "green")
                # 自动监控（延迟 1.5 秒，给 esptool 释放串口时间）
                if self.auto_monitor_var.get():
                    self._log("自动开始监控串口...", TAG_INFO)
                    self.root.after(1500, lambda: self._start_monitor())
            else:
                self._log("烧录失败，请查看上方 esptool 输出", TAG_ERROR)
                self._update_status("烧录失败", "red")

        except Exception as e:
            self._log(f"烧录过程发生未知异常: {e}", TAG_ERROR)
            self._update_status("烧录失败", "red")
        finally:
            self._is_flashing = False
            try:
                self.root.after(0, lambda: self._set_buttons_state(flashing=False))
            except Exception:
                pass  # root 可能已销毁（窗口关闭后烧录线程仍在运行）

    def _run_esptool(self, argv_list):
        """进程内调用 esptool.main(argv)，实时输出日志到日志区。

        返回 True 成功，False 失败。
        esptool 成功时不返回，失败时抛 SystemExit 或其他异常。
        """
        try:
            import esptool
        except ImportError:
            self._log("esptool 未安装，无法烧录", TAG_ERROR)
            return False

        stream = _EsptoolStream(self._log)
        try:
            # 重定向 stdout/stderr 到日志区
            with redirect_stdout(stream), redirect_stderr(stream):
                esptool.main(argv_list)
            stream.flush()
            return True
        except SystemExit as e:
            stream.flush()
            # esptool 成功也可能 sys.exit(0)
            code = e.code
            if code is None or code == 0:
                return True
            else:
                self._log(f"esptool 退出码: {code}", TAG_ERROR)
                return False
        except Exception as e:
            stream.flush()
            # esptool 用自定义异常报告错误，常见如 ESPFatalChipNotFoundError
            err_msg = str(e)
            if err_msg and err_msg != "":
                self._log(f"esptool 异常: {err_msg}", TAG_ERROR)
            else:
                self._log(f"esptool 异常: {type(e).__name__}", TAG_ERROR)
            return False

    # ================================================================
    # 串口监控
    # ================================================================
    def _start_monitor(self):
        """启动串口监控。"""
        # 防止烧录完成后延迟回调（after 1500ms）在新烧录期间触发串口冲突
        if self._is_flashing:
            self._log("烧录进行中，跳过自动监控", TAG_INFO)
            return
        if self._is_monitoring:
            self._log("监控已在运行", TAG_INFO)
            return
        if not SERIAL_AVAILABLE:
            self._log("pyserial 未安装，无法监控串口", TAG_ERROR)
            return

        port = self._get_selected_port_name()
        if not port:
            messagebox.showerror("错误", "请先选择串口！")
            return

        self._stop_monitor_event.clear()
        self._is_monitoring = True
        self.stop_monitor_btn.config(state=tk.NORMAL)
        self._update_status("监控中", "blue")

        self._monitor_thread = threading.Thread(
            target=self._monitor_thread_func,
            args=(port,),
            daemon=True
        )
        self._monitor_thread.start()

    def _monitor_thread_func(self, port):
        """串口监控线程函数。"""
        self._log(f"打开串口 {port} @ {MONITOR_BAUDRATE} 进行监控...", TAG_INFO)
        try:
            self._serial_port = serial.Serial(
                port,
                baudrate=MONITOR_BAUDRATE,
                timeout=1,
                write_timeout=1
            )
            self._log(f"串口 {port} 已打开", TAG_SUCCESS)

            buffer = b""
            while not self._stop_monitor_event.is_set():
                try:
                    if self._serial_port.in_waiting > 0:
                        data = self._serial_port.read(self._serial_port.in_waiting)
                    else:
                        # 没有数据时少量休眠避免 CPU 占用
                        data = self._serial_port.read(1)
                    if not data:
                        continue
                    buffer += data
                    # 按行处理
                    while b"\n" in buffer:
                        line_bytes, buffer = buffer.split(b"\n", 1)
                        try:
                            line = line_bytes.decode("utf-8", errors="replace").rstrip("\r")
                        except Exception:
                            line = str(line_bytes)
                        if line:
                            self._process_serial_line(line)
                except serial.SerialException as e:
                    if self._stop_monitor_event.is_set():
                        break
                    self._log(f"串口异常: {e}", TAG_ERROR)
                    break
                except Exception as e:
                    if self._stop_monitor_event.is_set():
                        break
                    self._log(f"监控读取异常: {e}", TAG_ERROR)
                    # 短暂休眠避免异常循环
                    time.sleep(0.2)

        except serial.SerialException as e:
            self._log(f"打开串口失败: {e}", TAG_ERROR)
            self._update_status("监控失败", "red")
        except Exception as e:
            self._log(f"监控线程异常: {e}", TAG_ERROR)
            self._update_status("监控失败", "red")
        finally:
            if self._serial_port is not None and self._serial_port.is_open:
                try:
                    self._serial_port.close()
                except Exception:
                    pass
            self._serial_port = None
            self._is_monitoring = False
            try:
                self.root.after(0, lambda: self.stop_monitor_btn.config(state=tk.DISABLED))
            except Exception:
                pass  # root 可能已销毁
            if not self._stop_monitor_event.is_set():
                self._update_status("监控结束", "blue")
            else:
                self._update_status("已停止监控", "blue")
            self._log("串口监控已结束", TAG_INFO)

    def _process_serial_line(self, line):
        """处理一行串口数据：写入日志并匹配二维码。"""
        # 写入日志区
        self._log(line, TAG_SERIAL)

        # 匹配 Matter 二维码
        match = QR_REGEX.search(line)
        if match:
            mt_code = match.group(1)
            self._log(f"★ 检测到 Matter 二维码: {mt_code}", TAG_HIGHLIGHT)
            # 通过 after 调度 GUI 更新
            self.root.after(0, lambda m=mt_code: self._on_qr_detected(m))

        # 匹配手动配对码
        manual_match = MANUAL_CODE_REGEX.search(line)
        if manual_match:
            code = manual_match.group(1)
            self.root.after(0, lambda c=code: self.manual_code_var.set(c))
            self._log(f"★ 检测到手动配对码: {code}", TAG_HIGHLIGHT)

    def _on_qr_detected(self, mt_code):
        """在主线程中处理二维码检测。"""
        self.mt_code_var.set(mt_code)
        self._generate_and_display_qrcode(mt_code)
        # 自动保存二维码到 qr_codes 文件夹（同一 MT 码不重复保存）
        self._auto_save_qrcode(mt_code)

    def _auto_save_qrcode(self, mt_code):
        """自动保存二维码 PNG 到 qr_codes 文件夹。

        同一 MT 码只保存一次，避免重复文件堆积。
        """
        if mt_code == self._last_saved_mt_code:
            return  # 同一二维码已保存过

        if not QRCODE_AVAILABLE or self._current_pil_image is None:
            return

        try:
            os.makedirs(QR_SAVE_DIR, exist_ok=True)
            # 文件名：时间戳_MT码安全化.png
            safe_name = re.sub(r'[\\/:*?"<>|]', "_", mt_code)
            timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            file_path = os.path.join(QR_SAVE_DIR, f"{timestamp}_{safe_name}.png")
            self._current_pil_image.save(file_path, "PNG")
            self._last_saved_mt_code = mt_code
            self._log(f"二维码已自动保存: {file_path}", TAG_SUCCESS)
        except Exception as e:
            self._log(f"自动保存二维码失败: {e}", TAG_ERROR)

    def _on_stop_monitor(self):
        """停止监控按钮回调。"""
        self._stop_monitor()

    def _stop_monitor(self, wait_thread=True):
        """停止串口监控。

        只设置停止标志并取消阻塞读，串口关闭由监控线程的 finally 统一处理，
        避免主线程和监控线程同时操作串口导致的竞态条件。

        参数:
            wait_thread: True 时等待监控线程退出（用于烧录前确保串口已释放），
                         False 时仅设置标志（用于窗口关闭等非阻塞场景）。
        """
        if not self._is_monitoring:
            return
        self._stop_monitor_event.set()
        self._log("正在停止监控...", TAG_INFO)
        # 取消阻塞读，让监控线程跳出循环自行关闭串口
        if self._serial_port is not None and self._serial_port.is_open:
            try:
                self._serial_port.cancel_read()
            except Exception:
                pass
        # 等待监控线程退出，确保串口已释放（避免烧录时串口占用）
        if wait_thread and self._monitor_thread and self._monitor_thread.is_alive():
            self._monitor_thread.join(timeout=3)
            if self._monitor_thread.is_alive():
                self._log("警告: 监控线程未在超时内退出", TAG_ERROR)

    # ================================================================
    # 二维码生成与显示
    # ================================================================
    def _generate_and_display_qrcode(self, mt_code):
        """生成二维码并显示。"""
        if not QRCODE_AVAILABLE:
            self._log("qrcode / Pillow 未安装，无法生成二维码", TAG_ERROR)
            return
        try:
            qr = qrcode.QRCode(
                version=1,
                error_correction=qrcode.constants.ERROR_CORRECT_M,
                box_size=8,
                border=QR_BORDER,
            )
            qr.add_data(mt_code)
            qr.make(fit=True)
            pil_img = qr.make_image(fill_color="black", back_color="white").convert("RGB")

            # 调整到显示尺寸（兼容 Pillow 9.0+ 和旧版本）
            try:
                resample = Image.Resampling.LANCZOS
            except AttributeError:
                resample = Image.LANCZOS
            pil_img = pil_img.resize((QR_DISPLAY_SIZE, QR_DISPLAY_SIZE), resample)

            self._current_pil_image = pil_img
            self._current_qr_image = ImageTk.PhotoImage(pil_img)
            # 注意：不要在 Label 上设置 width/height（单位是字符），用 image 替换文本即可
            self.qr_label.config(
                image=self._current_qr_image,
                text=""
            )
            self._log("二维码已生成并显示", TAG_SUCCESS)
        except Exception as e:
            self._log(f"生成二维码失败: {e}", TAG_ERROR)

    def _on_print_qrcode(self):
        """打印二维码按钮回调。"""
        if self._current_pil_image is None:
            messagebox.showwarning("提示", "暂无二维码可打印，请先等待设备输出。")
            return

        if WIN32PRINT_AVAILABLE and win32print is not None and win32ui is not None:
            try:
                self._print_with_win32(self._current_pil_image)
                self._log("已发送至打印机", TAG_SUCCESS)
                return
            except Exception as e:
                self._log(f"打印失败，将回退到保存 PNG: {e}", TAG_ERROR)
        else:
            self._log("win32print 不可用，将回退到保存 PNG", TAG_INFO)

        # 回退：保存为 PNG
        self._on_save_qrcode()

    def _print_with_win32(self, pil_image):
        """使用 Windows API 打印二维码（A4 居中）。"""
        # 获取默认打印机
        printer_name = win32print.GetDefaultPrinter()
        self._log(f"使用打印机: {printer_name}", TAG_INFO)

        dc = win32ui.CreateDC()
        try:
            dc.CreatePrinterDC(printer_name)

            # 可打印区域（像素单位）
            printable_w = dc.GetDeviceCaps(110)  # PHYSICALWIDTH
            printable_h = dc.GetDeviceCaps(111)  # PHYSICALHEIGHT
            offset_x = dc.GetDeviceCaps(112)     # PHYSICALOFFSETX
            offset_y = dc.GetDeviceCaps(113)     # PHYSICALOFFSETY

            # 二维码目标尺寸：A4 较短边的一半，保证足够大且清晰
            target_size = min(printable_w, printable_h) // 2
            x = (printable_w - target_size) // 2 - offset_x
            y = (printable_h - target_size) // 2 - offset_y

            dc.StartDoc("Matter Bridge QR Code")
            dc.StartPage()

            # 使用 PIL.ImageWin.Dib 绘制到打印机 DC（正确 API）
            # dib.draw() 接受 (left, top, right, bottom) 矩形，自动缩放图像
            dib = ImageWin.Dib(pil_image.convert("RGB"))
            dib.draw(dc.GetHandleOutput(), (x, y, x + target_size, y + target_size))

            dc.EndPage()
            dc.EndDoc()
        finally:
            dc.DeleteDC()

    def _on_save_qrcode(self):
        """保存二维码按钮回调。"""
        if self._current_pil_image is None:
            messagebox.showwarning("提示", "暂无二维码可保存，请先等待设备输出。")
            return
        mt_code = self.mt_code_var.get() or "matter_qr"
        # 文件名安全化
        safe_name = re.sub(r'[\\/:*?"<>|]', "_", mt_code)
        default_name = f"{safe_name}.png"
        file_path = filedialog.asksaveasfilename(
            title="保存二维码",
            defaultextension=".png",
            filetypes=[("PNG 图片", "*.png")],
            initialfile=default_name
        )
        if not file_path:
            return
        try:
            self._current_pil_image.save(file_path, "PNG")
            self._log(f"二维码已保存: {file_path}", TAG_SUCCESS)
            messagebox.showinfo("成功", f"二维码已保存到:\n{file_path}")
        except Exception as e:
            self._log(f"保存二维码失败: {e}", TAG_ERROR)
            messagebox.showerror("错误", f"保存失败:\n{e}")

    # ================================================================
    # 日志与状态
    # ================================================================
    def _log(self, message, tag=TAG_INFO):
        """向日志区写入一条消息（线程安全，通过 after 调度）。"""
        timestamp = datetime.now().strftime("[%H:%M:%S]")
        full = f"{timestamp} {message}"
        # 通过 after 在主线程更新
        try:
            self.root.after(0, lambda f=full, t=tag: self._append_log(f, t))
        except Exception:
            pass  # root 可能已销毁（窗口关闭后线程仍在运行）

    def _append_log(self, text, tag):
        """在主线程中实际写入日志。"""
        try:
            self.log_text.configure(state=tk.NORMAL)
            self.log_text.insert(tk.END, text + "\n", tag)
            # 限制日志最大行数，删除旧行防止内存无限增长
            line_count = int(self.log_text.index('end-1c').split('.')[0])
            if line_count > LOG_MAX_LINES:
                # 删除最旧的 (line_count - LOG_MAX_LINES) 行
                self.log_text.delete('1.0', f'{line_count - LOG_MAX_LINES}.0')
            self.log_text.see(tk.END)
            self.log_text.configure(state=tk.DISABLED)
        except Exception:
            pass

    def _update_status(self, text, color="black"):
        """更新状态标签。"""
        try:
            self.root.after(0, lambda t=text, c=color: self._do_update_status(t, c))
        except Exception:
            pass  # root 可能已销毁

    def _do_update_status(self, text, color):
        """在主线程中更新状态。"""
        try:
            self.status_label.config(text=f"状态: {text}", foreground=color)
        except Exception:
            pass

    def _set_buttons_state(self, flashing):
        """根据烧录状态启用/禁用按钮和配置控件。"""
        try:
            state = tk.DISABLED if flashing else tk.NORMAL
            self.flash_btn.config(state=state)
            self.erase_flash_btn.config(state=state)
            # port_combo 和 baud_combo 原始状态为 readonly，恢复时保持 readonly
            combo_state = tk.DISABLED if flashing else "readonly"
            self.port_combo.config(state=combo_state)
            self.refresh_port_btn.config(state=state)
            self.baud_combo.config(state=combo_state)
            self.firmware_dir_entry.config(state=state)
            self.browse_dir_btn.config(state=state)
            self.erase_checkbtn.config(state=state)
            self.auto_monitor_checkbtn.config(state=state)
        except Exception:
            pass

    # ================================================================
    # 设置持久化
    # ================================================================
    def _load_settings(self):
        """从配置文件加载设置。"""
        try:
            if os.path.isfile(SETTINGS_FILE):
                with open(SETTINGS_FILE, "r", encoding="utf-8") as f:
                    data = json.load(f)
                if "port" in data:
                    # 保存的是纯端口名（如 COM5），加载时暂存，扫描串口后匹配
                    self._saved_port = data["port"]
                if "baudrate" in data:
                    try:
                        saved_baud = int(data["baudrate"])
                        # 仅接受下拉框中可选的波特率，避免旧配置含 2000000 等
                        self.baud_var.set(saved_baud if saved_baud in BAUDRATE_OPTIONS else DEFAULT_BAUDRATE)
                    except (ValueError, TypeError):
                        pass
                if "firmware_dir" in data:
                    self.firmware_dir_var.set(data["firmware_dir"])
                if "erase" in data:
                    self.erase_var.set(bool(data["erase"]))
                if "auto_monitor" in data:
                    self.auto_monitor_var.set(bool(data["auto_monitor"]))
        except (OSError, json.JSONDecodeError) as e:
            # 加载失败不致命
            print(f"加载设置失败: {e}")
        except Exception as e:
            print(f"加载设置未知异常: {e}")

    def _save_settings(self):
        """保存设置到配置文件。"""
        try:
            settings_dir = os.path.dirname(SETTINGS_FILE)
            if settings_dir and not os.path.isdir(settings_dir):
                os.makedirs(settings_dir, exist_ok=True)
            data = {
                "port": self._get_selected_port_name(),
                "baudrate": int(self.baud_var.get()) if self.baud_var.get() else DEFAULT_BAUDRATE,
                "firmware_dir": self.firmware_dir_var.get(),
                "erase": bool(self.erase_var.get()),
                "auto_monitor": bool(self.auto_monitor_var.get()),
            }
            with open(SETTINGS_FILE, "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False, indent=2)
        except Exception as e:
            print(f"保存设置失败: {e}")

    # ================================================================
    # 资源清理与关闭
    # ================================================================
    def _on_closing(self):
        """窗口关闭事件处理。"""
        # 烧录进行中时警告用户（esptool 无法真正中断，强制关闭可能导致固件不完整）
        if self._is_flashing:
            result = messagebox.askyesno(
                "警告",
                "烧录正在进行中！\n\n"
                "esptool 无法中途安全中断，强制关闭可能导致设备固件不完整。\n"
                "确定要强制关闭吗？",
                icon="warning"
            )
            if not result:
                return

        try:
            # 停止监控
            if self._is_monitoring:
                self._stop_monitor()
            # 停止烧录（仅设置标志，esptool 执行完当前阶段后检查）
            if self._is_flashing:
                self._stop_flash_event.set()

            # 等待线程结束（最多 2 秒）
            if self._monitor_thread and self._monitor_thread.is_alive():
                self._monitor_thread.join(timeout=2)
            if self._flash_thread and self._flash_thread.is_alive():
                self._flash_thread.join(timeout=2)

            # 关闭串口
            if self._serial_port is not None and self._serial_port.is_open:
                try:
                    self._serial_port.close()
                except Exception:
                    pass
        except Exception:
            pass
        finally:
            # 保存设置
            self._save_settings()
            # 销毁窗口
            try:
                self.root.destroy()
            except Exception:
                pass

    # ================================================================
    # 运行
    # ================================================================
    def run(self):
        """启动主循环。"""
        # 启动时检查依赖并提示
        missing = []
        if not SERIAL_AVAILABLE:
            missing.append("pyserial")
        if not QRCODE_AVAILABLE:
            missing.append("qrcode / Pillow")
        if missing:
            self._log(f"缺少依赖: {', '.join(missing)}（部分功能不可用）", TAG_ERROR)
        else:
            self._log("TuyaOpen Bridge 烧录工具已就绪", TAG_SUCCESS)

        if not WIN32PRINT_AVAILABLE:
            self._log("提示: pywin32 未安装，打印功能将回退到保存 PNG", TAG_INFO)

        self._log(f"应用目录: {APP_DIR}")
        self._log(f"配置文件: {SETTINGS_FILE}")

        # 检查 esptool 是否可用
        try:
            import esptool  # noqa: F401
            self._log("esptool 模块可用", TAG_SUCCESS)
        except ImportError:
            self._log("esptool 未安装，烧录功能不可用！请运行: pip install esptool", TAG_ERROR)

        self.root.mainloop()


# ====================================================================
# 程序入口
# ====================================================================
if __name__ == "__main__":
    app = FlashToolApp()
    app.run()
