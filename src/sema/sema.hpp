#pragma once
#include <string>
#include <vector>

#include "../ast.hpp"

namespace tl {

struct SemaResult {
    FnDecl* main_fn = nullptr;
    std::vector<std::string> unsafe_report;  // deklarasi & pemanggilan 'unsafe extern fn'
    std::vector<std::string> panic_report;   // setiap abort(...) / panic(...) eksplisit
    // program memakai defer/recover: panic (termasuk OOM & #max_depth) membuka frame lewat flag
    // unwind; tanpa itu panic langsung fatal (tanpa biaya)
    bool panic_mode = false;
};

// Resolusi nama, pemeriksaan tipe, dan pembuktian keamanan (rentang, inisialisasi,
// must-use, return) dalam satu pass yang sensitif terhadap alur.
SemaResult check_program(Program& prog, Diag& diag);

}  // namespace tl
