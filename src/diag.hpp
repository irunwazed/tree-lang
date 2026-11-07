#pragma once
#include <string>
#include <vector>

namespace tl {

struct SourceFile;

struct Loc {
    int line = 0;
    int col = 0;
    const SourceFile* file = nullptr;  // null = file utama
};

struct SourceFile {
    std::string path;
    std::string text;
    std::vector<size_t> line_starts;
    std::string pkg;  // import path package pemilik file ("" = package main / prelude)
    void index_lines();
    std::string line_text(int line) const;
};

// Nama internal package ("myapp/utils.Point") ditampilkan sebagai "utils.Point".
void register_pkg_display(const std::string& import_path);
std::string display_text(const std::string& s);

// Mengumpulkan error/warning; dicetak dengan lokasi file:baris:kolom dan cuplikan sumber.
class Diag {
public:
    explicit Diag(const SourceFile* file) : file_(file) {}
    void error(Loc loc, const std::string& msg, const std::string& help = "");
    void warning(Loc loc, const std::string& msg, const std::string& help = "");
    int error_count() const { return errors_; }
    // Mode senyap untuk analisis spekulatif (mis. iterasi invarian loop).
    void push_mute() { muted_++; }
    void pop_mute() { muted_--; }
    bool has_errors() const { return errors_ > 0; }

private:
    void emit(const char* kind, Loc loc, const std::string& msg, const std::string& help);
    const SourceFile* file_;
    int errors_ = 0;
    int muted_ = 0;
};

}  // namespace tl
