#pragma once
#include <memory>
#include <string>
#include <vector>

#include "ast.hpp"
#include "diag.hpp"

namespace tl {

// Hasil memuat program: semua file sumber (harus hidup selama kompilasi) dan sidecar C.
struct LoadedProgram {
    std::vector<std::unique_ptr<SourceFile>> files;
    std::vector<std::string> sidecars;  // <file>.c di samping setiap file .tree
    std::string module;                 // nama modul dari forest.json ("" = mode file tunggal)
    std::string main_dir;
};

// Memuat program dari file .tree atau folder.
//   - tanpa forest.json: file tunggal (mode skrip, tanpa import)
//   - dengan forest.json: folder program = package main, import dimuat rekursif
// Deklarasi package non-main diberi nama internal "<importpath>.Nama"; referensi berkualifikasi
// (utils.Add, utils.Point{...}, []utils.Point) ditulis ulang ke nama internal itu.
bool load_program(const std::string& input, Diag& diag, Program& out, LoadedProgram& lp);

// Folder library standar: import "std/http" → <dir>/http. Diisi driver (FOREST_STDLIB / di samping exe).
void set_stdlib_dir(const std::string& dir);

}  // namespace tl
