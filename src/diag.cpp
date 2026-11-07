#include "diag.hpp"

#include <cstdio>
#include <vector>

namespace tl {

void SourceFile::index_lines() {
    line_starts.clear();
    line_starts.push_back(0);
    for (size_t i = 0; i < text.size(); i++)
        if (text[i] == '\n') line_starts.push_back(i + 1);
}

std::string SourceFile::line_text(int line) const {
    if (line < 1 || line > (int)line_starts.size()) return "";
    size_t start = line_starts[line - 1];
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string s = text.substr(start, end - start);
    if (!s.empty() && s.back() == '\r') s.pop_back();
    return s;
}

namespace {
std::vector<std::pair<std::string, std::string>>& pkg_names() {
    static std::vector<std::pair<std::string, std::string>> v;
    return v;
}
}  // namespace

void register_pkg_display(const std::string& path) {
    std::string from = path + ".";
    for (auto& [f, t] : pkg_names())
        if (f == from) return;
    size_t slash = path.rfind('/');
    pkg_names().push_back({from, (slash == std::string::npos ? path : path.substr(slash + 1)) + "."});
}

std::string display_text(const std::string& s) {
    if (pkg_names().empty() || s.find('/') == std::string::npos) return s;
    std::string r = s;
    for (auto& [from, to] : pkg_names()) {
        size_t p = 0;
        while ((p = r.find(from, p)) != std::string::npos) {
            r.replace(p, from.size(), to);
            p += to.size();
        }
    }
    return r;
}

void Diag::error(Loc loc, const std::string& msg, const std::string& help) {
    if (muted_) return;
    errors_++;
    emit("error", loc, msg, help);
}

void Diag::warning(Loc loc, const std::string& msg, const std::string& help) {
    if (muted_) return;
    emit("warning", loc, msg, help);
}

void Diag::emit(const char* kind, Loc loc, const std::string& msg, const std::string& help) {
    const SourceFile* f = loc.file ? loc.file : file_;
    std::fprintf(stderr, "%s:%d:%d: %s: %s\n", f->path.c_str(), loc.line, loc.col, kind, display_text(msg).c_str());
    std::string src = f->line_text(loc.line);
    if (!src.empty()) {
        std::fprintf(stderr, "    %s\n    ", src.c_str());
        for (int i = 1; i < loc.col; i++) std::fputc(src[i - 1] == '\t' ? '\t' : ' ', stderr);
        std::fprintf(stderr, "^\n");
    }
    if (!help.empty()) std::fprintf(stderr, "  help: %s\n", display_text(help).c_str());
}

}  // namespace tl
