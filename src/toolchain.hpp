#pragma once
#include <string>
#include <vector>

namespace tl {

// Jalankan proses dan tunggu selesai. Mengembalikan exit code (-1 jika gagal start).
int run_process(const std::vector<std::string>& args);

std::string find_clang();
std::string find_runtime_dir(const std::string& argv0);
// Folder library standar (stdlib/): TREELANG_STDLIB, atau dicari naik dari lokasi treelang.exe.
std::string find_stdlib_dir(const std::string& argv0);
std::string temp_dir();
std::string exe_suffix();

struct BuildOptions {
    bool optimize = true;
    bool debug_info = false;
    bool verbose = false;
    std::vector<std::string> extra_sources;  // file C tambahan (FFI)
};

// .ll + runtime → executable. Mengembalikan true jika sukses.
bool link_executable(const std::string& ll_path, const std::string& runtime_dir, const std::string& out_exe,
                     const BuildOptions& opts);

}  // namespace tl
