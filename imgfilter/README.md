# Multithreaded Image Filter (C++/POSIX threads + Python GUI)

An OS-project demo: a **multithreaded C++ backend** (POSIX `pthread`) applies
image filters in parallel, controlled by a **Python Tkinter GUI**. The GUI
never does the filtering itself — it only launches the C++ engine as a
subprocess and displays its progress and result.

```
[Python Tkinter GUI]  --launches as subprocess-->  [C++ filter_engine]
        |                                                   |
        | reads stdout (PROGRESS / THREAD / DONE lines)     | pthread_create x N
        | shows live progress bar + log                     | each thread filters
        | displays before/after preview                      | a horizontal band
        v                                                   v
   on-screen result                                  shared output buffer
                                                       (disjoint writes -> race-free)
```

## Project layout

```
imgfilter/
├── build.sh                 # compiles the C++ backend
├── src/
│   ├── filter_engine.cpp    # the multithreaded engine (this is the OS part)
│   └── ppm.hpp              # minimal PPM/PGM image I/O, no external deps
└── gui/
    └── gui.py                # Tkinter front-end, drives filter_engine via subprocess
```

## Build

Requires `g++` with C++11+ and `pthread` (standard on any Linux/macOS dev box;
on Windows use WSL or MinGW with `-lpthread`).

```bash
./build.sh
# or manually:
cd src && g++ -O2 -pthread -o filter_engine filter_engine.cpp
```

## Run the GUI

Requires Python 3 with **Pillow** (`pip install Pillow`). Tkinter ships with
most standard Python installs (`python3-tk` on Debian/Ubuntu if missing).

```bash
cd gui
python3 gui.py
```

1. **Open Image...** — pick any PNG/JPG/BMP. It's converted to PPM for the engine.
2. Choose a **filter** and **thread count**.
3. **Run Filter** — the backend log shows each thread starting/finishing and
   a live progress bar; the result appears next to the original when done.

## Run the backend directly (no GUI)

```bash
./src/filter_engine input.ppm output.ppm blur 4
```

Arguments: `<input.ppm|.pgm> <output.ppm|.pgm> <filter> <num_threads>`
Filters: `grayscale`, `blur`, `edge`, `invert`, `sharpen`

The engine only reads/writes binary PPM (`P6`, RGB) or PGM (`P5`, grayscale).
The GUI handles PNG/JPG conversion via Pillow so you can still load any
common format from the file picker — the C++ backend itself stays
dependency-free (no libpng/libjpeg linking required).

## OS concepts this project demonstrates

- **Thread creation & lifecycle** — `pthread_create` / `pthread_join`, one
  thread per row-band of the image (`filter_engine.cpp`, `main()`).
- **Work partitioning** — the image height is divided as evenly as possible
  across N threads (remainder rows distributed to the first few threads).
- **Shared resource + mutual exclusion** — every thread updates a shared
  `g_rowsDone` counter as it finishes batches of rows. This is guarded by a
  `pthread_mutex_t` (`g_progressMutex`) — a textbook example of a critical
  section protecting a shared variable from a race condition.
- **Race-free parallel writes** — pixel data itself needs *no* locking
  because each thread writes only to its own disjoint row-range of the
  output buffer. This is verified directly: running the same filter with
  1, 4, and 8 threads produces **byte-identical output** every time (see
  Testing below) — proof the parallel decomposition is correct.
- **IPC between processes** — the Python GUI and C++ engine are separate
  processes; they communicate over a pipe (`stdout`), a simple
  line-oriented protocol (`INFO ...`, `THREAD i START/DONE`,
  `PROGRESS n`, `DONE time_ms=...`, `ERROR ...`).
- **Producer/consumer inside the GUI** — the GUI reads the subprocess's
  stdout on a background Python thread (so the window never freezes) and
  hands lines to the Tk main thread through a thread-safe `queue.Queue`.

## Testing performed

- Built with `-Wall`, zero warnings.
- All 5 filters run successfully at 1, 4, and 8 threads on a generated test
  image; **output is byte-for-byte identical across thread counts** for
  every filter — the strongest evidence the row-partitioning has no race
  conditions.
- Verified visually: grayscale desaturates correctly, edge-detect highlights
  the boundaries between regions, invert/blur/sharpen all produce visibly
  correct results.
- Edge cases tested: thread count far exceeding image height (clamps
  correctly), 1×1 pixel image, true single-channel grayscale PGM input,
  missing input file, unknown filter name (now fails fast with a clear
  error instead of silently "succeeding").
- The exact subprocess-launch + stdout-parsing logic the GUI uses was
  tested standalone (without Tkinter) to confirm progress values arrive
  monotonically and reach 100%, and that the PPM→PNG round-trip for the
  preview works.

**Note on timing demos:** parallel speedup depends on having genuine
multiple CPU cores available at runtime. On a constrained/virtualized
single-core environment you may not see wall-clock speedup from more
threads (correctness is unaffected) — for your demo, run on a normal
multi-core laptop/desktop and use a large image (e.g. 2000×1500) so the
per-thread work is large enough for the speedup to be visible over thread
-creation overhead.

## Possible extensions for a writeup

- Replace the simple row-mutex progress reporting with a `pthread_barrier_t`
  multi-pass filter (e.g. blur passes) to show barrier synchronization too.
- Add a `--benchmark` mode that runs the same filter at 1, 2, 4, 8 threads
  back-to-back and prints a speedup table.
- Swap stdout-pipe IPC for POSIX shared memory (`shm_open`) between GUI and
  engine to demonstrate a second IPC mechanism for comparison.
