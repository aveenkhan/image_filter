#!/usr/bin/env python3
"""
gui.py -- Tkinter front-end for the multithreaded C++ image filter engine.

Responsibilities of THIS file (deliberately kept thin):
    - Let the user pick an image (any format Pillow can read: png/jpg/bmp/...)
    - Convert it to PPM (the C++ engine's native format) using Pillow
    - Launch ./filter_engine as a subprocess with the chosen filter + thread count
    - Stream the subprocess's stdout (PROGRESS / THREAD / DONE lines) into the
      GUI in real time, without freezing the window
    - Convert the PPM result back to PNG and show a before/after preview

All actual filtering and threading happens in the C++ backend (filter_engine).
This file does NOT do image processing itself -- it is a controller + viewer.

Because the GUI must stay responsive while the backend runs, the subprocess
is launched and read from a background Python thread (using Python's
`threading` module), and results are pushed back to the Tk main thread via
a thread-safe `queue.Queue`. This mirrors a classic producer/consumer
pattern: the reader thread produces log lines, the Tk mainloop consumes them.
"""

import os
import queue
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

try:
    from PIL import Image, ImageTk
except ImportError:
    print("This GUI requires Pillow. Install it with:\n    pip install Pillow")
    sys.exit(1)

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ENGINE_PATH = os.path.join(SCRIPT_DIR, "..", "src", "filter_engine")
WORK_DIR = os.path.join(SCRIPT_DIR, "_work")
os.makedirs(WORK_DIR, exist_ok=True)

FILTERS = ["grayscale", "blur", "edge", "invert", "sharpen"]
PREVIEW_SIZE = (340, 340)


class FilterApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Multithreaded Image Filter (C++/pthreads backend)")
        self.geometry("760x560")
        self.minsize(680, 520)

        self.input_path = None
        self.input_ppm_path = os.path.join(WORK_DIR, "input.ppm")
        self.output_ppm_path = os.path.join(WORK_DIR, "output.ppm")
        self.output_png_path = os.path.join(WORK_DIR, "output.png")

        self.log_queue = queue.Queue()
        self.worker_thread = None
        self.original_photo = None  # keep references so Tk doesn't GC images
        self.result_photo = None

        self._build_ui()
        self._poll_queue()

    # ---------------- UI construction ----------------

    def _build_ui(self):
        top = ttk.Frame(self, padding=10)
        top.pack(side=tk.TOP, fill=tk.X)

        ttk.Button(top, text="Open Image...", command=self.on_open_image).pack(side=tk.LEFT)
        self.path_label = ttk.Label(top, text="No image loaded", foreground="#555")
        self.path_label.pack(side=tk.LEFT, padx=10)

        controls = ttk.Frame(self, padding=(10, 0, 10, 10))
        controls.pack(side=tk.TOP, fill=tk.X)

        ttk.Label(controls, text="Filter:").grid(row=0, column=0, sticky="w")
        self.filter_var = tk.StringVar(value=FILTERS[0])
        filter_menu = ttk.Combobox(
            controls, textvariable=self.filter_var, values=FILTERS, state="readonly", width=12
        )
        filter_menu.grid(row=0, column=1, padx=(5, 20))

        ttk.Label(controls, text="Threads:").grid(row=0, column=2, sticky="w")
        self.thread_var = tk.IntVar(value=4)
        thread_spin = ttk.Spinbox(controls, from_=1, to=64, textvariable=self.thread_var, width=5)
        thread_spin.grid(row=0, column=3, padx=(5, 20))

        self.run_button = ttk.Button(
            controls, text="Run Filter", command=self.on_run_filter, state=tk.DISABLED
        )
        self.run_button.grid(row=0, column=4, padx=(0, 20))

        self.progress = ttk.Progressbar(controls, mode="determinate", length=200, maximum=100)
        self.progress.grid(row=0, column=5, sticky="ew")
        controls.columnconfigure(5, weight=1)

        # Before / after preview panes
        previews = ttk.Frame(self, padding=10)
        previews.pack(side=tk.TOP, fill=tk.BOTH, expand=True)
        previews.columnconfigure(0, weight=1)
        previews.columnconfigure(1, weight=1)
        previews.rowconfigure(1, weight=1)

        ttk.Label(previews, text="Original").grid(row=0, column=0)
        ttk.Label(previews, text="Filtered").grid(row=0, column=1)

        self.original_canvas = tk.Label(previews, background="#222", relief=tk.SUNKEN)
        self.original_canvas.grid(row=1, column=0, sticky="nsew", padx=5, pady=5)
        self.result_canvas = tk.Label(previews, background="#222", relief=tk.SUNKEN)
        self.result_canvas.grid(row=1, column=1, sticky="nsew", padx=5, pady=5)

        # Backend log (shows THREAD/PROGRESS/DONE lines from the C++ engine)
        log_frame = ttk.LabelFrame(self, text="Backend log (pthreads activity)", padding=5)
        log_frame.pack(side=tk.BOTTOM, fill=tk.X, padx=10, pady=(0, 10))
        self.log_text = tk.Text(log_frame, height=8, state=tk.DISABLED, font=("Courier", 9))
        self.log_text.pack(fill=tk.X)

    # ---------------- Image loading ----------------

    def on_open_image(self):
        path = filedialog.askopenfilename(
            title="Choose an image",
            filetypes=[("Images", "*.png *.jpg *.jpeg *.bmp *.ppm *.pgm"), ("All files", "*.*")],
        )
        if not path:
            return

        try:
            img = Image.open(path).convert("RGB")
        except Exception as e:
            messagebox.showerror("Error", f"Could not open image:\n{e}")
            return

        self.input_path = path
        self.path_label.config(text=os.path.basename(path))

        # Convert to binary PPM (P6) for the C++ backend -- no dependency
        # needed on the C++ side for PNG/JPEG decoding.
        img.save(self.input_ppm_path, format="PPM")

        self._show_image(img, self.original_canvas, is_original=True)
        self.result_canvas.config(image="")
        self.run_button.config(state=tk.NORMAL)
        self._log_clear()

    def _show_image(self, pil_img, label_widget, is_original):
        preview = pil_img.copy()
        preview.thumbnail(PREVIEW_SIZE)
        photo = ImageTk.PhotoImage(preview)
        label_widget.config(image=photo)
        if is_original:
            self.original_photo = photo  # prevent garbage collection
        else:
            self.result_photo = photo

    # ---------------- Running the C++ backend ----------------

    def on_run_filter(self):
        if not self.input_path:
            return
        if self.worker_thread and self.worker_thread.is_alive():
            return  # already running

        if not os.path.isfile(ENGINE_PATH):
            messagebox.showerror(
                "Backend not found",
                f"Couldn't find the compiled C++ engine at:\n{ENGINE_PATH}\n\n"
                "Build it first with:\n  cd src && g++ -O2 -pthread -o filter_engine filter_engine.cpp",
            )
            return

        self.run_button.config(state=tk.DISABLED)
        self.progress["value"] = 0
        self._log_clear()

        filt = self.filter_var.get()
        threads = max(1, int(self.thread_var.get()))

        self.worker_thread = threading.Thread(
            target=self._run_backend, args=(filt, threads), daemon=True
        )
        self.worker_thread.start()

    def _run_backend(self, filt, threads):
        """Runs in a background thread so the Tk mainloop never blocks."""
        cmd = [ENGINE_PATH, self.input_ppm_path, self.output_ppm_path, filt, str(threads)]
        self.log_queue.put(("LOG", f"$ {' '.join(cmd)}"))

        try:
            proc = subprocess.Popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1
            )
        except Exception as e:
            self.log_queue.put(("ERROR", f"Failed to launch backend: {e}"))
            self.log_queue.put(("REENABLE", None))
            return

        for line in proc.stdout:
            line = line.rstrip("\n")
            if not line:
                continue
            self.log_queue.put(("LOG", line))
            if line.startswith("PROGRESS"):
                try:
                    pct = int(line.split()[1])
                    self.log_queue.put(("PROGRESS", pct))
                except (IndexError, ValueError):
                    pass

        rc = proc.wait()
        if rc == 0:
            self.log_queue.put(("PROGRESS", 100))
            self.log_queue.put(("FINISHED", None))
        else:
            self.log_queue.put(("ERROR", f"Backend exited with code {rc}"))

        self.log_queue.put(("REENABLE", None))

    # ---------------- Queue polling (runs on the Tk main thread) ----------------

    def _poll_queue(self):
        try:
            while True:
                kind, payload = self.log_queue.get_nowait()
                if kind == "LOG":
                    self._log_append(payload)
                elif kind == "PROGRESS":
                    self.progress["value"] = payload
                elif kind == "ERROR":
                    self._log_append(f"[ERROR] {payload}")
                    messagebox.showerror("Backend error", payload)
                elif kind == "FINISHED":
                    self._on_filter_finished()
                elif kind == "REENABLE":
                    self.run_button.config(state=tk.NORMAL)
        except queue.Empty:
            pass
        self.after(50, self._poll_queue)

    def _on_filter_finished(self):
        try:
            result_img = Image.open(self.output_ppm_path).convert("RGB")
            result_img.save(self.output_png_path, format="PNG")
        except Exception as e:
            self._log_append(f"[ERROR] Could not load filtered output: {e}")
            return
        self._show_image(result_img, self.result_canvas, is_original=False)
        self._log_append("Done. Output saved to: " + self.output_png_path)

    # ---------------- Logging helpers ----------------

    def _log_append(self, text):
        self.log_text.config(state=tk.NORMAL)
        self.log_text.insert(tk.END, text + "\n")
        self.log_text.see(tk.END)
        self.log_text.config(state=tk.DISABLED)

    def _log_clear(self):
        self.log_text.config(state=tk.NORMAL)
        self.log_text.delete("1.0", tk.END)
        self.log_text.config(state=tk.DISABLED)


if __name__ == "__main__":
    app = FilterApp()
    app.mainloop()
