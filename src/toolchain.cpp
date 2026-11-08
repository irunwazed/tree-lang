#include "toolchain.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace tl {

#ifdef _WIN32
// Quoting argumen sesuai aturan CommandLineToArgvW / CRT.
static std::string quote_arg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : a) {
        if (c == '\\') {
            backslashes++;
        } else if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
            backslashes = 0;
        } else {
            out.append(backslashes, '\\');
            out += c;
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}

int run_process(const std::vector<std::string>& args) {
    std::string cmd;
    for (size_t i = 0; i < args.size(); i++) {
        if (i) cmd += ' ';
        cmd += quote_arg(args[i]);
    }
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

std::string exe_suffix() { return ".exe"; }
#else
int run_process(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) return -1;
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

std::string exe_suffix() { return ""; }
#endif

static bool on_path(const std::string& exe) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::string p = path;
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find(sep, start);
        if (end == std::string::npos) end = p.size();
        fs::path cand = fs::path(p.substr(start, end - start)) / (exe + exe_suffix());
        std::error_code ec;
        if (!p.substr(start, end - start).empty() && fs::exists(cand, ec)) return true;
        start = end + 1;
    }
    return false;
}

std::string find_clang() {
    if (const char* env = std::getenv("FOREST_CLANG")) return env;
    if (on_path("clang")) return "clang";
#ifdef _WIN32
    const char* candidates[] = {"C:\\Program Files\\LLVM\\bin\\clang.exe"};
#else
    const char* candidates[] = {"/usr/bin/clang", "/usr/local/bin/clang", "/opt/homebrew/opt/llvm/bin/clang"};
#endif
    for (const char* c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec)) return c;
    }
    return "";
}

std::string find_runtime_dir(const std::string& argv0) {
    if (const char* env = std::getenv("FOREST_RUNTIME")) return env;
    std::error_code ec;
    fs::path exe = fs::absolute(argv0, ec);
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) exe = buf;
#endif
    fs::path dir = exe.parent_path();
    for (int up = 0; up < 4; up++) {
        fs::path cand = dir / "runtime" / "tl_runtime.c";
        if (fs::exists(cand, ec)) return (dir / "runtime").string();
        dir = dir.parent_path();
    }
    return "";
}

std::string find_stdlib_dir(const std::string& argv0) {
    if (const char* env = std::getenv("FOREST_STDLIB")) return env;
    std::error_code ec;
    fs::path exe = fs::absolute(argv0, ec);
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) exe = buf;
#endif
    fs::path dir = exe.parent_path();
    for (int up = 0; up < 4; up++) {
        if (fs::is_directory(dir / "stdlib", ec)) return (dir / "stdlib").string();
        dir = dir.parent_path();
    }
    return "";
}

// Folder kerja sementara: FOREST_TMP, atau 'tmp' di samping forest.exe.
// (Tidak memakai %TEMP% agar tidak membebani drive sistem.)
std::string temp_dir() {
    std::error_code ec;
    fs::path t;
    if (const char* env = std::getenv("FOREST_TMP")) {
        t = env;
    } else {
#ifdef _WIN32
        char buf[MAX_PATH];
        DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
        t = (n > 0 && n < MAX_PATH) ? fs::path(buf).parent_path() / "tmp" : fs::temp_directory_path(ec) / "forest";
#else
        t = fs::read_symlink("/proc/self/exe", ec).parent_path() / "tmp";
        if (ec) t = fs::temp_directory_path(ec) / "forest";
#endif
    }
    fs::create_directories(t, ec);
    return fs::absolute(t, ec).string();
}

// clang menulis file objek sementara ke TEMP/TMP; arahkan ke folder kerja kita.
static void redirect_child_temp() {
    std::string t = temp_dir();
#ifdef _WIN32
    SetEnvironmentVariableA("TEMP", t.c_str());
    SetEnvironmentVariableA("TMP", t.c_str());
#else
    setenv("TMPDIR", t.c_str(), 1);
#endif
}

bool link_executable(const std::string& ll_path, const std::string& runtime_dir, const std::string& out_exe,
                     const BuildOptions& opts) {
    std::string clang = find_clang();
    if (clang.empty()) {
        std::fprintf(stderr,
                     "error: clang not found. Install LLVM (e.g. 'winget install LLVM.LLVM') or set FOREST_CLANG.\n");
        return false;
    }
    std::vector<std::string> args = {clang,
                                     opts.optimize ? "-O2" : "-O0",
                                     "-Wno-override-module",
                                     ll_path,
                                     (fs::path(runtime_dir) / "tl_runtime.c").string(),
                                     "-o",
                                     out_exe};
    if (opts.debug_info) args.push_back("-g");
    for (auto& c : opts.extra_sources) args.push_back(c);
#ifdef _WIN32
    args.push_back("-D_CRT_SECURE_NO_WARNINGS");
#endif
    if (opts.verbose) {
        for (auto& a : args) std::fprintf(stderr, "%s ", a.c_str());
        std::fprintf(stderr, "\n");
    }
    redirect_child_temp();
    int rc = run_process(args);
    if (rc != 0) {
        std::fprintf(stderr, "error: clang failed (exit code %d)\n", rc);
        return false;
    }
    return true;
}

}  // namespace tl
