#pragma once
#include <string>

#include "ast.hpp"
#include "sema/sema.hpp"

namespace tl {

struct CodegenOptions {
    // Sisipkan assert runtime untuk setiap properti yang sudah dibuktikan sema
    // (overflow, bagi nol, shift, cast). Untuk menangkap bug di compiler sendiri.
    bool debug_asserts = false;
};

std::string emit_llvm(const Program& prog, const SemaResult& sema, const CodegenOptions& opts);

}  // namespace tl
