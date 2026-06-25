// ppm.hpp
// Minimal PPM (P6, binary RGB) and PGM (P5, binary grayscale) image I/O.
// No external dependencies -- keeps the C++ backend buildable with just g++.
#ifndef PPM_HPP
#define PPM_HPP

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

struct Image {
    int width = 0;
    int height = 0;
    int channels = 3; // 3 = RGB (PPM), 1 = grayscale (PGM)
    std::vector<unsigned char> data; // size = width * height * channels

    unsigned char &at(int x, int y, int c) {
        return data[(static_cast<size_t>(y) * width + x) * channels + c];
    }
    unsigned char at(int x, int y, int c) const {
        return data[(static_cast<size_t>(y) * width + x) * channels + c];
    }
};

// Skips whitespace and '#' comments in a PNM header, per the format spec.
inline void skipWhitespaceAndComments(std::ifstream &f) {
    int c;
    while ((c = f.peek()) != EOF) {
        if (std::isspace(c)) {
            f.get();
        } else if (c == '#') {
            std::string line;
            std::getline(f, line);
        } else {
            break;
        }
    }
}

inline Image loadPNM(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open input file: " + path);

    std::string magic;
    f >> magic;
    if (magic != "P6" && magic != "P5") {
        throw std::runtime_error("Unsupported PNM format (need P6/PPM or P5/PGM): " + magic);
    }

    Image img;
    img.channels = (magic == "P6") ? 3 : 1;

    skipWhitespaceAndComments(f);
    f >> img.width;
    skipWhitespaceAndComments(f);
    f >> img.height;
    skipWhitespaceAndComments(f);
    int maxval;
    f >> maxval;
    f.get(); // consume the single whitespace char after maxval, per spec

    size_t total = static_cast<size_t>(img.width) * img.height * img.channels;
    img.data.resize(total);
    f.read(reinterpret_cast<char *>(img.data.data()), total);
    if (!f) throw std::runtime_error("Truncated or corrupt PNM file: " + path);

    return img;
}

inline void savePNM(const std::string &path, const Image &img) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open output file: " + path);

    f << (img.channels == 3 ? "P6" : "P5") << "\n"
      << img.width << " " << img.height << "\n255\n";
    f.write(reinterpret_cast<const char *>(img.data.data()), img.data.size());
}

#endif // PPM_HPP
