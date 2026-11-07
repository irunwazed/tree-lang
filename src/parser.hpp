#pragma once
#include <vector>

#include "ast.hpp"

namespace tl {

Program parse(const std::vector<Token>& toks, Diag& diag);

}  // namespace tl
