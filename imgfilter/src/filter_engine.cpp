// filter_engine.cpp
//
// Multithreaded image filtering engine (POSIX threads).
//
// Demonstrates core OS concepts:
//   - Thread creation/management via pthreads (pthread_create/join)
//   - Work partitioning: image split into horizontal row-bands, one band per thread
//   - Mutual exclusion: a pthread_mutex_t guards a shared progress counter
//     that every thread updates as it finishes rows (classic shared-resource
//     synchronization, even though the actual pixel writes are race-free
//     because each thread owns disjoint memory regions)
//   - Inter-process communication with the GUI: progress/timing/status are
//     printed to stdout as simple line-based messages the Python GUI parses
//
// Usage:
//   ./filter_engine <input.ppm|.pgm> <output.ppm|.pgm> <filter> <num_threads>
//
// Filters: grayscale, blur, edge, invert, sharpen
//
// Build: g++ -O2 -pthread -o filter_engine filter_engine.cpp

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <string>
#include <vector>

#include "ppm.hpp"

// ---------- Shared state across threads ----------

struct ThreadArgs {
    const Image *src;     // read-only shared input (safe: never written after load)
    Image *dst;           // shared output buffer; each thread writes disjoint rows
    int startRow;
    int endRow;            // [startRow, endRow)
    std::string filter;
    int threadId;
    bool ok = true;        // set to false by the worker if it hits an error
};

static bool isKnownFilter(const std::string &f) {
    return f == "grayscale" || f == "invert" || f == "blur" || f == "sharpen" || f == "edge";
}

// Progress is a genuinely shared, mutable resource -> needs the mutex.
static long g_rowsDone = 0;
static int g_totalRows = 0;
static pthread_mutex_t g_progressMutex = PTHREAD_MUTEX_INITIALIZER;

static void reportProgress(int rowsJustFinished) {
    pthread_mutex_lock(&g_progressMutex);
    g_rowsDone += rowsJustFinished;
    int pct = static_cast<int>(100.0 * g_rowsDone / g_totalRows);
    // Single line, machine-parseable by the Python GUI: "PROGRESS <pct>"
    printf("PROGRESS %d\n", pct);
    fflush(stdout);
    pthread_mutex_unlock(&g_progressMutex);
}

// ---------- Pixel-level filter kernels ----------

static inline unsigned char clampToByte(int v) {
    return static_cast<unsigned char>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// 3x3 convolution helper, clamps at image edges by repeating the border pixel.
static void applyKernel3x3(const Image &src, Image &dst, int x, int y,
                            const double k[3][3], double bias = 0.0) {
    int w = src.width, h = src.height;
    for (int c = 0; c < src.channels; ++c) {
        double sum = 0.0;
        for (int ky = -1; ky <= 1; ++ky) {
            for (int kx = -1; kx <= 1; ++kx) {
                int sx = std::min(std::max(x + kx, 0), w - 1);
                int sy = std::min(std::max(y + ky, 0), h - 1);
                sum += k[ky + 1][kx + 1] * src.at(sx, sy, c);
            }
        }
        dst.at(x, y, c) = clampToByte(static_cast<int>(sum + bias));
    }
}

static void filterGrayscaleRow(const Image &src, Image &dst, int y) {
    for (int x = 0; x < src.width; ++x) {
        if (src.channels == 1) {
            dst.at(x, y, 0) = src.at(x, y, 0);
            continue;
        }
        unsigned char r = src.at(x, y, 0), g = src.at(x, y, 1), b = src.at(x, y, 2);
        unsigned char gray = clampToByte(static_cast<int>(0.299 * r + 0.587 * g + 0.114 * b));
        dst.at(x, y, 0) = gray;
        dst.at(x, y, 1) = gray;
        dst.at(x, y, 2) = gray;
    }
}

static void filterInvertRow(const Image &src, Image &dst, int y) {
    for (int x = 0; x < src.width; ++x)
        for (int c = 0; c < src.channels; ++c)
            dst.at(x, y, c) = 255 - src.at(x, y, c);
}

static void filterBlurRow(const Image &src, Image &dst, int y) {
    static const double k[3][3] = {
        {1 / 16.0, 2 / 16.0, 1 / 16.0},
        {2 / 16.0, 4 / 16.0, 2 / 16.0},
        {1 / 16.0, 2 / 16.0, 1 / 16.0}};
    for (int x = 0; x < src.width; ++x) applyKernel3x3(src, dst, x, y, k);
}

static void filterSharpenRow(const Image &src, Image &dst, int y) {
    static const double k[3][3] = {
        {0, -1, 0},
        {-1, 5, -1},
        {0, -1, 0}};
    for (int x = 0; x < src.width; ++x) applyKernel3x3(src, dst, x, y, k);
}

static void filterEdgeRow(const Image &src, Image &dst, int y) {
    // Simple Laplacian edge-detect kernel.
    static const double k[3][3] = {
        {-1, -1, -1},
        {-1, 8, -1},
        {-1, -1, -1}};
    for (int x = 0; x < src.width; ++x) applyKernel3x3(src, dst, x, y, k, 0.0);
}

// ---------- Thread entry point ----------

void *workerThread(void *argPtr) {
    ThreadArgs *args = static_cast<ThreadArgs *>(argPtr);
    const Image &src = *args->src;
    Image &dst = *args->dst;

    printf("THREAD %d START rows[%d,%d)\n", args->threadId, args->startRow, args->endRow);
    fflush(stdout);

    void (*rowFn)(const Image &, Image &, int) = nullptr;
    if (args->filter == "grayscale") rowFn = filterGrayscaleRow;
    else if (args->filter == "invert") rowFn = filterInvertRow;
    else if (args->filter == "blur") rowFn = filterBlurRow;
    else if (args->filter == "sharpen") rowFn = filterSharpenRow;
    else if (args->filter == "edge") rowFn = filterEdgeRow;
    else {
        fprintf(stderr, "Unknown filter: %s\n", args->filter.c_str());
        return nullptr;
    }

    // Each thread only ever touches rows in [startRow, endRow) of dst,
    // and only reads from src -- disjoint write regions mean no data race
    // on pixel data itself. The mutex below protects ONLY the shared
    // progress counter, not the image buffer.
    const int batch = 16; // report progress in small batches, not every row
    int sinceReport = 0;
    for (int y = args->startRow; y < args->endRow; ++y) {
        rowFn(src, dst, y);
        if (++sinceReport == batch) {
            reportProgress(sinceReport);
            sinceReport = 0;
        }
    }
    if (sinceReport > 0) reportProgress(sinceReport);

    printf("THREAD %d DONE\n", args->threadId);
    fflush(stdout);
    return nullptr;
}

// ---------- main ----------

int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr,
                "Usage: %s <input.ppm|.pgm> <output.ppm|.pgm> <filter> <num_threads>\n"
                "Filters: grayscale, blur, edge, invert, sharpen\n",
                argv[0]);
        return 1;
    }

    std::string inputPath = argv[1];
    std::string outputPath = argv[2];
    std::string filter = argv[3];
    int numThreads = std::atoi(argv[4]);
    if (numThreads < 1) numThreads = 1;

    if (!isKnownFilter(filter)) {
        fprintf(stderr, "ERROR Unknown filter '%s'. Choose from: grayscale, blur, edge, invert, sharpen\n",
                filter.c_str());
        return 1;
    }

    Image src;
    try {
        src = loadPNM(inputPath);
    } catch (const std::exception &e) {
        fprintf(stderr, "ERROR %s\n", e.what());
        return 1;
    }

    Image dst;
    dst.width = src.width;
    dst.height = src.height;
    dst.channels = src.channels;
    dst.data.resize(src.data.size());

    // Don't spawn more threads than there are rows to work on.
    numThreads = std::min(numThreads, std::max(1, src.height));

    g_totalRows = src.height;
    g_rowsDone = 0;

    printf("INFO image=%dx%d channels=%d filter=%s threads=%d\n",
           src.width, src.height, src.channels, filter.c_str(), numThreads);
    fflush(stdout);

    std::vector<pthread_t> threads(numThreads);
    std::vector<ThreadArgs> args(numThreads);

    int rowsPerThread = src.height / numThreads;
    int remainder = src.height % numThreads;
    int currentRow = 0;

    auto t0 = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < numThreads; ++i) {
        int rows = rowsPerThread + (i < remainder ? 1 : 0);
        args[i] = ThreadArgs{&src, &dst, currentRow, currentRow + rows, filter, i};
        currentRow += rows;

        int rc = pthread_create(&threads[i], nullptr, workerThread, &args[i]);
        if (rc != 0) {
            fprintf(stderr, "ERROR pthread_create failed for thread %d (rc=%d)\n", i, rc);
            return 1;
        }
    }

    for (int i = 0; i < numThreads; ++i) {
        pthread_join(threads[i], nullptr);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    try {
        savePNM(outputPath, dst);
    } catch (const std::exception &e) {
        fprintf(stderr, "ERROR %s\n", e.what());
        return 1;
    }

    printf("DONE time_ms=%.2f output=%s\n", ms, outputPath.c_str());
    fflush(stdout);
    return 0;
}
