# Multithreaded Image Filter

A multithreaded image filtering engine written in C++ (POSIX threads), controlled by a Python Tkinter GUI. Built as an OS systems project to demonstrate thread creation, data-parallel work partitioning, and mutex-protected shared state.
---

## Overview

The C++ backend splits an image into horizontal row-bands and filters each band on its own POSIX thread — a classic data-parallel (SPMD) decomposition where every thread runs identical code over disjoint data. The Python GUI is a thin controller: it launches the compiled backend as a subprocess, streams its progress in real time, and displays the before/after result. No image processing happens in Python — all filtering logic lives in C++.

```
[Python Tkinter GUI]  --launches as subprocess-->  [C++ filter_engine]
        |                                                   |
        | reads stdout (PROGRESS / THREAD / DONE lines)     | pthread_create x N
        | shows live progress bar + log                     | each thread filters
        | displays before/after preview                      | its own row-band
```

## Features

- **5 filters**: grayscale, blur, edge detection, invert, sharpen
- **Configurable thread count** at runtime
- **Live progress reporting** from the C++ backend to the GUI via stdout
- **Mutex-protected shared progress counter** — a concrete example of a critical section
- **Race-free parallel writes** — verified by checksum: output is byte-identical regardless of thread count
- Zero external C++ dependencies (custom PPM/PGM I/O, no libpng/libjpeg linking required)

## Tech stack

| Layer | Tech |
|---|---|
| Filtering engine | C++11, POSIX threads (`pthread`) |
| GUI | Python 3, Tkinter, Pillow |
| IPC | stdout pipe between GUI and engine subprocess |

## Getting started

### Prerequisites
- `g++` with C++11 support and `pthread`
- Python 3 with `Pillow` (`pip install Pillow`)
- `tkinter` (often a separate OS package, e.g. `sudo apt install python3-tk` on Debian/Ubuntu)

### Build

```bash
./build.sh
```

### Run

```bash
cd gui
python3 gui.py
```


## OS concepts demonstrated

- **Thread lifecycle**: `pthread_create` / `pthread_join`
- **Data parallelism**: image rows partitioned evenly across N threads, each running the same filter function over disjoint data
- **Mutual exclusion**: a `pthread_mutex_t` guards the one genuinely shared resource — a progress counter all threads update
- **Inter-process communication**: GUI and engine are separate processes, communicating over a stdout pipe with a simple line-based protocol
- **Producer/consumer pattern**: a background Python thread reads engine output and hands it to the GUI's main thread via a `queue.Queue`, keeping the UI responsive

