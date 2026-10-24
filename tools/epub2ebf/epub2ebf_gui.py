#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
epub2ebf 图形界面 —— 把 EPUB 转成本项目的 EBF 书籍文件夹

运行：
    python tools\\epub2ebf\\epub2ebf_gui.py
    （把后缀改成 .pyw 再用 pythonw 启动就没有黑色控制台窗口）

界面上的实用功能：
    - 选文件单本转换 / 选文件夹批量转换
    - 跳过前 N 章（丢掉简介、前言、导航这类开头页）
    - 去掉正文开头与章名重复的那一行
    - 段落之间空一行
    - 覆盖已存在的输出目录
    - 进度条 + 实时日志

转换逻辑全部在 epub2ebf.py 里，这个文件只管界面。
"""

from __future__ import annotations

import queue
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

sys.path.insert(0, str(Path(__file__).resolve().parent))
import epub2ebf                                            # noqa: E402

UI_FONT = ("Microsoft YaHei UI", 9)
PAD = 8


class ToolTip:
    """鼠标悬停提示"""

    def __init__(self, widget: tk.Widget, text: str):
        self.widget = widget
        self.text = text
        self.tip: tk.Toplevel | None = None
        widget.bind("<Enter>", self._show, add="+")
        widget.bind("<Leave>", self._hide, add="+")
        widget.bind("<ButtonPress>", self._hide, add="+")

    def _show(self, _event=None) -> None:
        if self.tip is not None:
            return
        x = self.widget.winfo_rootx() + 16
        y = self.widget.winfo_rooty() + self.widget.winfo_height() + 4
        self.tip = tk.Toplevel(self.widget)
        self.tip.wm_overrideredirect(True)
        self.tip.wm_geometry(f"+{x}+{y}")
        tk.Label(self.tip, text=self.text, justify="left", background="#ffffe0",
                 relief="solid", borderwidth=1, font=UI_FONT, padx=6, pady=3).pack()

    def _hide(self, _event=None) -> None:
        if self.tip is not None:
            self.tip.destroy()
            self.tip = None


class App:
    def __init__(self, root: tk.Tk):
        self.root = root
        root.title("EPUB → EBF 转换工具")
        root.geometry("660x620")
        root.minsize(580, 500)
        root.option_add("*Font", UI_FONT)

        self.in_path = tk.StringVar()
        self.out_dir = tk.StringVar()
        self.skip_n = tk.IntVar(value=0)
        self.opt_force = tk.BooleanVar(value=True)
        self.opt_blank = tk.BooleanVar(value=False)
        self.opt_dedup = tk.BooleanVar(value=False)
        self.status = tk.StringVar(value="就绪")

        self.queue: queue.Queue = queue.Queue()
        self.busy = False

        self._build()
        self.root.after(80, self._drain)

    # ---------- 界面 ----------

    def _build(self) -> None:
        outer = ttk.Frame(self.root, padding=PAD)
        outer.pack(fill="both", expand=True)
        outer.columnconfigure(1, weight=1)
        outer.rowconfigure(6, weight=1)

        # 输入
        ttk.Label(outer, text="EPUB 文件 / 目录:").grid(row=0, column=0, sticky="w", pady=4)
        entry = ttk.Entry(outer, textvariable=self.in_path)
        entry.grid(row=0, column=1, sticky="ew", padx=4, pady=4)
        btn_file = ttk.Button(outer, text="选文件", width=8, command=self.pick_file)
        btn_file.grid(row=0, column=2, pady=4)
        btn_dir = ttk.Button(outer, text="选文件夹", width=9, command=self.pick_dir)
        btn_dir.grid(row=0, column=3, padx=(4, 0), pady=4)
        ToolTip(btn_file, "选择单个 EPUB 文件")
        ToolTip(btn_dir, "选择文件夹，批量转换里面所有的 EPUB（含子目录）")

        # 输出
        ttk.Label(outer, text="输出目录:").grid(row=1, column=0, sticky="w", pady=4)
        ttk.Entry(outer, textvariable=self.out_dir).grid(row=1, column=1, sticky="ew",
                                                        padx=4, pady=4)
        btn_out = ttk.Button(outer, text="浏览", width=8, command=self.pick_out)
        btn_out.grid(row=1, column=2, pady=4)
        ToolTip(btn_out, "生成的 <书名>.ebf 文件夹放在哪里\n留空 = 放在 EPUB 旁边（批量时放在输入目录）")

        ttk.Label(outer, text="留空则与 EPUB 同目录；生成的 <书名>.ebf 拷进 SD 卡 /books/ 即可",
                  foreground="#777777").grid(row=2, column=0, columnspan=4, sticky="w", pady=(0, 6))

        # 选项
        opts = ttk.LabelFrame(outer, text="选项", padding=PAD)
        opts.grid(row=3, column=0, columnspan=4, sticky="ew", pady=4)

        skip_row = ttk.Frame(opts)
        skip_row.grid(row=0, column=0, sticky="w")
        ttk.Label(skip_row, text="跳过前").pack(side="left")
        spin = ttk.Spinbox(skip_row, from_=0, to=99, width=5, textvariable=self.skip_n,
                           state="readonly")
        spin.pack(side="left", padx=4)
        ttk.Label(skip_row, text="章（丢掉简介、前言、导航这类开头页）").pack(side="left")
        ToolTip(spin, "转换后按顺序丢掉最前面的 N 章。\n用来去掉 EPUB 里的简介、前言、目录页。\n"
                      "被跳过的章名会打印在下面的日志里。")

        cb_force = ttk.Checkbutton(opts, text="覆盖已存在的输出目录", variable=self.opt_force)
        cb_force.grid(row=1, column=0, sticky="w", pady=(6, 0))
        ToolTip(cb_force, "不勾选时，若 <书名>.ebf 已存在会直接报错退出")

        cb_blank = ttk.Checkbutton(opts, text="段落之间空一行", variable=self.opt_blank)
        cb_blank.grid(row=2, column=0, sticky="w", pady=(2, 0))
        ToolTip(cb_blank, "默认每段占一行、段间不留空行，小屏更省地方\n勾上后段间空一行，看着更松")

        cb_dedup = ttk.Checkbutton(opts, text="去掉正文开头重复的章标题",
                                   variable=self.opt_dedup)
        cb_dedup.grid(row=3, column=0, sticky="w", pady=(2, 0))
        ToolTip(cb_dedup, "EPUB 每章正文开头常有一个 <h1> 章节名，和目录里的章名重复。\n"
                          "勾上后把正文第一行删掉，避免阅读时重复显示。")

        # 进度
        self.progress = ttk.Progressbar(outer, mode="determinate", maximum=100)
        self.progress.grid(row=4, column=0, columnspan=4, sticky="ew", pady=(10, 2))

        # 开始
        self.btn_start = ttk.Button(outer, text="开始转换", command=self.start)
        self.btn_start.grid(row=5, column=0, columnspan=4, pady=8)

        # 日志
        log_box = ttk.LabelFrame(outer, text="日志", padding=4)
        log_box.grid(row=6, column=0, columnspan=4, sticky="nsew")
        log_box.columnconfigure(0, weight=1)
        log_box.rowconfigure(0, weight=1)

        self.log_text = tk.Text(log_box, wrap="word", height=12, relief="flat",
                                background="#fbfbfb", state="normal")
        self.log_text.grid(row=0, column=0, sticky="nsew")
        self.log_text.bind("<Key>", self._readonly_key)

        bar = ttk.Scrollbar(log_box, command=self.log_text.yview)
        bar.grid(row=0, column=1, sticky="ns")
        self.log_text.config(yscrollcommand=bar.set)

        self.log_text.tag_config("info", foreground="#222222")
        self.log_text.tag_config("warn", foreground="#b06000")
        self.log_text.tag_config("error", foreground="#c00000")
        self.log_text.tag_config("ok", foreground="#0a7a0a")

        ttk.Label(outer, textvariable=self.status, foreground="#555555").grid(
            row=7, column=0, columnspan=4, sticky="w", pady=(6, 0))

    def _readonly_key(self, event):
        """日志区只读，但放行 Ctrl+C / Ctrl+A 这类组合键"""
        if event.state & 0x4:
            return None
        return "break"

    # ---------- 事件 ----------

    def pick_file(self) -> None:
        name = filedialog.askopenfilename(title="选择 EPUB",
                                          filetypes=[("EPUB 电子书", "*.epub"),
                                                     ("所有文件", "*.*")])
        if name:
            self.in_path.set(name)

    def pick_dir(self) -> None:
        name = filedialog.askdirectory(title="选择包含 EPUB 的文件夹")
        if name:
            self.in_path.set(name)

    def pick_out(self) -> None:
        name = filedialog.askdirectory(title="选择输出目录")
        if name:
            self.out_dir.set(name)

    def start(self) -> None:
        if self.busy:
            return

        src = Path(self.in_path.get().strip())
        if not src.exists():
            messagebox.showerror("错误", "请先选择 EPUB 文件或包含 EPUB 的文件夹")
            return

        files = epub2ebf.collect_inputs([str(src)])
        if not files:
            messagebox.showerror("错误", f"{src} 里没有找到 EPUB 文件")
            return

        out = self.out_dir.get().strip()
        if out:
            out_root: Path | None = Path(out)
        elif src.is_dir():
            out_root = src                 # 批量模式默认放回输入目录
        else:
            out_root = None                # 放 EPUB 旁边

        opt = epub2ebf.Options(
            out_root=out_root,
            force=self.opt_force.get(),
            blank_line=self.opt_blank.get(),
            skip=max(0, int(self.skip_n.get())),
            dedup_title=self.opt_dedup.get(),
            log=lambda m: self.queue.put(("log", m)),
            warn=lambda m: self.queue.put(("warn", m)),
            progress=lambda d, t, m: self.queue.put(("progress", (d, t, m))),
        )

        self.busy = True
        self.btn_start.config(state="disabled")
        self.progress["value"] = 0
        self.log_text.delete("1.0", "end")
        self.status.set("开始转换…")

        threading.Thread(target=self._work, args=(files, opt), daemon=True).start()

    def _work(self, files: list[Path], opt: epub2ebf.Options) -> None:
        """后台线程：只往队列里丢消息，不碰任何控件"""
        ok = 0
        for i, f in enumerate(files):
            if len(files) > 1:
                self.queue.put(("log", f"\n===== [{i + 1}/{len(files)}] {f.name} ====="))
            else:
                self.queue.put(("log", f"[转换] {f.name}"))
            if epub2ebf.convert_one(f, opt):
                ok += 1
                self.queue.put(("log", "  完成"))
            self.queue.put(("progress", (1, 1, "")))
        self.queue.put(("done", (ok, len(files))))

    # ---------- 主线程消息泵 ----------

    def _drain(self) -> None:
        try:
            while True:
                kind, payload = self.queue.get_nowait()
                if kind == "log":
                    self._append(payload, "info")
                elif kind == "warn":
                    self._append(payload, "error" if payload.startswith("[失败]") else "warn")
                elif kind == "progress":
                    done, total, msg = payload
                    self.progress["value"] = (done / total * 100) if total else 0
                    if msg:
                        self.status.set(msg)
                elif kind == "done":
                    self._finish(*payload)
        except queue.Empty:
            pass
        self.root.after(80, self._drain)

    def _append(self, msg: str, tag: str) -> None:
        self.log_text.insert("end", msg + "\n", tag)
        self.log_text.see("end")

    def _finish(self, ok: int, total: int) -> None:
        self.busy = False
        self.btn_start.config(state="normal")
        self.progress["value"] = 100

        if total == 1:
            if ok:
                self.status.set("转换完成")
                self._append("转换完成。", "ok")
                messagebox.showinfo("完成", "转换完成！\n把生成的 .ebf 文件夹拷到 SD 卡的 /books/ 下即可。")
            else:
                self.status.set("转换失败")
                messagebox.showerror("失败", "转换失败，详情见日志。")
        else:
            self.status.set(f"完成：成功 {ok} / 失败 {total - ok}")
            self._append(f"批量完成：成功 {ok}，失败 {total - ok}", "ok" if ok == total else "warn")
            (messagebox.showinfo if ok == total else messagebox.showwarning)(
                "完成", f"批量转换完成\n成功 {ok} 个\n失败 {total - ok} 个")


def main() -> None:
    try:                                   # 高 DPI 屏上别糊（必须在建窗口之前设）
        from ctypes import windll
        windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass

    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
