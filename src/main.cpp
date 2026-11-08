// treelang: driver CLI untuk compiler tree-lang.
//
//   treelang run   <file.tree> [-O0] [--debug] [-v]   kompilasi lalu jalankan
//   treelang build <file.tree> [-o out.exe] [...]     buat executable
//   treelang check <file.tree>                        hanya periksa (tanpa codegen)
//   treelang emit  <file.tree> [-o out.ll]            tulis LLVM IR
//   treelang <file.tree>                              sama dengan 'run'
//
// Input boleh berupa folder (atau file di dalam folder) yang memiliki forest.json di atasnya:
// semua file .tree di folder itu menjadi package main, dan import dimuat dari root modul.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "codegen_llvm.hpp"
#include "config.hpp"
#include "diag.hpp"
#include "lexer.hpp"
#include "package.hpp"
#include "parser.hpp"
#include "prelude.hpp"
#include "sema/sema.hpp"
#include "toolchain.hpp"

namespace fs = std::filesystem;
using namespace tl;

namespace {

void usage() {
    std::printf(
        "%s %s - compiler for %s\n\n"
        "usage:\n"
        "  %s run   <file%s|folder> [options]   compile and run\n"
        "  %s build <file%s|folder> [options]   build an executable\n"
        "  %s check <file%s|folder>             type-check and prove safety only\n"
        "  %s emit  <file%s|folder> [-o out.ll] write LLVM IR\n"
        "  a folder (or a file inside it) under a forest.json is a module: all .tree files of the\n"
        "  folder form package main, and 'import \"<module>/<dir>\"' loads other folders\n\n"
        "options:\n"
        "  -o <path>   output path\n"
        "  -O0         disable optimization\n"
        "  --debug     re-verify every compile-time proof at runtime (catches compiler bugs), -O0, debug info\n"
        "  -v          print the clang command line\n"
        "  --c <file>  compile and link an extra C source (also: <file>.c next to the program)\n"
        "  --unsafe-report  list every 'unsafe extern fn' and every call to one (check)\n"
        "  --panic-report   list every explicit abort(...) and panic(...) (check)\n"
        "  -- <args>   pass the remaining arguments to the program (run)\n",
        kCliName, kVersion, kLangName, kCliName, kSourceExt, kCliName, kSourceExt, kCliName, kSourceExt, kCliName,
        kSourceExt);
}

struct Options {
    std::string command = "run";
    std::string input;
    std::string output;
    bool optimize = true;
    bool debug = false;
    bool verbose = false;
    bool unsafe_report = false;
    bool panic_report = false;
    std::vector<std::string> c_sources;
    std::vector<std::string> prog_args;
};

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// Frontend lengkap: muat package → parse → sema → LLVM IR. Kosong jika ada error.
bool compile_to_ll(const Options& o, std::string& ll, bool want_codegen, LoadedProgram& lp) {
    static SourceFile input_src;  // lokasi tanpa file (mis. "tidak ada main") dilaporkan pada input
    input_src.path = o.input;
    Diag diag(&input_src);
    Program prog;
    if (!load_program(o.input, diag, prog, lp)) {
        if (diag.has_errors()) std::fprintf(stderr, "%d error(s)\n", diag.error_count());
        return false;
    }
    // prelude: fungsi generik standar; fungsi dengan nama yang sama di program menggantikannya
    static SourceFile prelude_src;
    if (prelude_src.text.empty()) {
        prelude_src.path = kPreludePath;
        prelude_src.text = kPrelude;
        prelude_src.index_lines();
    }
    {
        std::vector<Token> ptoks = lex(prelude_src, diag);
        Program pre = parse(ptoks, diag);
        std::set<std::string> user;
        for (auto& f : prog.fns)
            if (!f->has_receiver) user.insert(f->name);
        for (auto& f : pre.fns)
            if (!user.count(f->name)) prog.fns.push_back(std::move(f));
    }
    if (diag.has_errors()) {
        std::fprintf(stderr, "%d error(s)\n", diag.error_count());
        return false;
    }
    SemaResult sema = check_program(prog, diag);
    if (diag.has_errors()) {
        std::fprintf(stderr, "%d error(s)\n", diag.error_count());
        return false;
    }
    if (o.unsafe_report) {
        std::printf("unsafe (code the compiler cannot check):\n");
        if (sema.unsafe_report.empty()) std::printf("  none\n");
        for (auto& l : sema.unsafe_report) std::printf("  %s\n", display_text(l).c_str());
    }
    if (o.panic_report) {
        std::printf("explicit aborts and panics:\n");
        if (sema.panic_report.empty()) std::printf("  none\n");
        for (auto& l : sema.panic_report) std::printf("  %s\n", display_text(l).c_str());
    }
    if (!want_codegen) return true;
    CodegenOptions cg;
    cg.debug_asserts = o.debug;
    ll = emit_llvm(prog, sema, cg);
    return true;
}

bool write_file(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << data;
    return (bool)f;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    std::vector<std::string> args(argv + 1, argv + argc);
    size_t i = 0;
    if (args.empty()) {
        usage();
        return 2;
    }
    if (args[0] == "--help" || args[0] == "-h") {
        usage();
        return 0;
    }
    if (args[0] == "--version") {
        std::printf("%s %s\n", kCliName, kVersion);
        return 0;
    }
    if (args[0] == "run" || args[0] == "build" || args[0] == "check" || args[0] == "emit") {
        o.command = args[0];
        i = 1;
    }
    for (; i < args.size(); i++) {
        const std::string& a = args[i];
        if (a == "-o" && i + 1 < args.size()) o.output = args[++i];
        else if (a == "-O0") o.optimize = false;
        else if (a == "-O2") o.optimize = true;
        else if (a == "--debug") o.debug = true;
        else if (a == "-v") o.verbose = true;
        else if (a == "--unsafe-report") o.unsafe_report = true;
        else if (a == "--panic-report") o.panic_report = true;
        else if (a == "--c" && i + 1 < args.size()) o.c_sources.push_back(args[++i]);
        else if (a == "--") {
            o.prog_args.assign(args.begin() + (long)i + 1, args.end());
            break;
        }
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "error: unknown option '%s'\n", a.c_str());
            return 2;
        } else if (o.input.empty()) o.input = a;
        else {
            std::fprintf(stderr, "error: more than one input file\n");
            return 2;
        }
    }
    if (o.input.empty()) {
        std::fprintf(stderr, "error: no input file\n");
        return 2;
    }
    if (o.debug) o.optimize = false;

    set_stdlib_dir(find_stdlib_dir(argv[0]));
    std::string ll;
    LoadedProgram lp;
    if (o.command == "check") return compile_to_ll(o, ll, false, lp) ? 0 : 1;
    if (!compile_to_ll(o, ll, true, lp)) return 1;

    // nama keluaran: file → nama file; folder → nama folder
    fs::path in(o.input);
    {
        std::error_code dec;
        if (fs::is_directory(in, dec)) {
            fs::path abs = fs::absolute(in, dec).lexically_normal();
            if (abs.filename().empty()) abs = abs.parent_path();
            in = abs / (abs.filename().string() + kSourceExt);
        }
    }
    if (o.command == "emit") {
        std::string out = o.output.empty() ? fs::path(in).replace_extension(".ll").string() : o.output;
        if (!write_file(out, ll)) {
            std::fprintf(stderr, "error: cannot write '%s'\n", out.c_str());
            return 1;
        }
        return 0;
    }

    std::string runtime = find_runtime_dir(argv[0]);
    if (runtime.empty()) {
        std::fprintf(stderr, "error: runtime directory not found (set TREELANG_RUNTIME)\n");
        return 1;
    }
    std::string stem = in.stem().string() + "-" + std::to_string(getpid());
    fs::path tmp = fs::path(temp_dir());
    fs::path ll_path = tmp / (stem + ".ll");
    if (!write_file(ll_path.string(), ll)) {
        std::fprintf(stderr, "error: cannot write '%s'\n", ll_path.string().c_str());
        return 1;
    }
    BuildOptions bo;
    bo.optimize = o.optimize;
    bo.debug_info = o.debug;
    bo.verbose = o.verbose;
    bo.extra_sources = o.c_sources;
    // sidecar: <file>.c di samping setiap file .tree (semua package) ikut dikompilasi
    for (auto& s : lp.sidecars) bo.extra_sources.push_back(s);

    std::error_code ec;
    if (o.command == "build") {
        std::string out = o.output.empty() ? fs::path(in).replace_extension(exe_suffix()).string() : o.output;
        bool ok = link_executable(ll_path.string(), runtime, out, bo);
        fs::remove(ll_path, ec);
        return ok ? 0 : 1;
    }
    // run
    fs::path exe = tmp / (stem + exe_suffix());
    bool ok = link_executable(ll_path.string(), runtime, exe.string(), bo);
    fs::remove(ll_path, ec);
    if (!ok) return 1;
    std::fflush(stdout);
    std::vector<std::string> cmd = {exe.string()};
    cmd.insert(cmd.end(), o.prog_args.begin(), o.prog_args.end());
    int rc = run_process(cmd);
    fs::remove(exe, ec);
    fs::remove(fs::path(exe).replace_extension(".pdb"), ec);
    fs::remove(fs::path(exe).replace_extension(".ilk"), ec);
    return rc;
}
