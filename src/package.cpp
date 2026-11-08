// Package & import ala Go.
//
//   myapp/forest.json      { "module": "myapp" }
//   myapp/main.tree        package main
//                          import "myapp/utils"
//   myapp/utils/*.tree     package utils      (satu folder = satu package)
//
// Strategi: deklarasi top-level package non-main diberi nama internal unik "<importpath>.Nama"
// (mis. "myapp/utils.Add"), referensi berkualifikasi "utils.Add" ditulis ulang ke nama itu, lalu
// semua package digabung menjadi satu Program. Sema mencari nama tanpa kualifikasi lewat package
// file pemakainya (SourceFile::pkg), sehingga nama privat tetap tertutup di package-nya.

#include "package.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "lexer.hpp"
#include "parser.hpp"

namespace fs = std::filesystem;

namespace tl {

namespace {

constexpr const char* kManifest = "forest.json";

std::string& stdlib_dir() {
    static std::string d;
    return d;
}

bool is_std(const std::string& path) { return path == "std" || path.compare(0, 4, "std/") == 0; }

bool read_text(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool is_ident(const std::string& s) {
    if (s.empty() || !(std::isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
    for (char c : s)
        if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
    return true;
}

bool exported(const std::string& name) { return !name.empty() && std::isupper((unsigned char)name[0]); }

std::string last_segment(const std::string& path) {
    size_t s = path.rfind('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}

// Path tampilan: relatif terhadap folder kerja bila memungkinkan.
std::string show_path(const fs::path& p) {
    std::error_code ec;
    fs::path r = fs::proximate(p, ec);
    return (ec ? p : r).generic_string();
}

// ---------------- JSON (cukup untuk forest.json) ----------------

struct Json {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    std::string s;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;
    Loc loc;
    const Json* get(const std::string& k) const {
        for (auto& [key, v] : obj)
            if (key == k) return &v;
        return nullptr;
    }
};

class JsonParser {
public:
    JsonParser(const SourceFile& f, Diag& d) : f_(f), t_(f.text), d_(d) {}
    bool parse(Json& out) {
        if (t_.compare(0, 3, "\xEF\xBB\xBF") == 0) i_ = 3;
        ws();
        if (!value(out)) return false;
        ws();
        if (i_ < t_.size()) return fail("unexpected text after the JSON value");
        return true;
    }

private:
    Loc loc() const {
        auto it = std::upper_bound(f_.line_starts.begin(), f_.line_starts.end(), i_);
        int line = (int)(it - f_.line_starts.begin());
        size_t start = f_.line_starts[line - 1];
        return Loc{line, (int)(i_ - start) + 1, &f_};
    }
    bool fail(const std::string& msg) {
        d_.error(loc(), "invalid forest.json: " + msg);
        return false;
    }
    void ws() {
        while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\t' || t_[i_] == '\n' || t_[i_] == '\r')) i_++;
    }
    bool lit(const char* w) {
        size_t n = std::char_traits<char>::length(w);
        if (t_.compare(i_, n, w) != 0) return false;
        i_ += n;
        return true;
    }
    bool string(std::string& out) {
        i_++;  // "
        while (i_ < t_.size() && t_[i_] != '"') {
            char c = t_[i_++];
            if ((unsigned char)c < 0x20) {
                i_--;
                return fail("control character in string");
            }
            if (c == '\\') {
                if (i_ >= t_.size()) break;
                char e = t_[i_++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        if (i_ + 4 > t_.size()) return fail("bad \\u escape");
                        unsigned v = 0;
                        for (int k = 0; k < 4; k++) {
                            char h = t_[i_++];
                            v <<= 4;
                            if (h >= '0' && h <= '9') v |= h - '0';
                            else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
                            else return fail("bad \\u escape");
                        }
                        if (v < 0x80) out += (char)v;
                        else if (v < 0x800) {
                            out += (char)(0xC0 | (v >> 6));
                            out += (char)(0x80 | (v & 0x3F));
                        } else {
                            out += (char)(0xE0 | (v >> 12));
                            out += (char)(0x80 | ((v >> 6) & 0x3F));
                            out += (char)(0x80 | (v & 0x3F));
                        }
                        break;
                    }
                    default: i_--; return fail("unknown escape in string");
                }
            } else {
                out += c;
            }
        }
        if (i_ >= t_.size()) return fail("unterminated string");
        i_++;
        return true;
    }
    bool value(Json& v) {
        if (++depth_ > 64) return fail("nested too deeply");
        v.loc = loc();
        bool ok = value_inner(v);
        depth_--;
        return ok;
    }
    bool value_inner(Json& v) {
        if (i_ >= t_.size()) return fail("unexpected end of file");
        char c = t_[i_];
        if (c == '{') {
            v.kind = Json::Obj;
            i_++;
            ws();
            if (i_ < t_.size() && t_[i_] == '}') {
                i_++;
                return true;
            }
            for (;;) {
                ws();
                if (i_ >= t_.size() || t_[i_] != '"') return fail("expected a key string");
                std::string k;
                if (!string(k)) return false;
                ws();
                if (i_ >= t_.size() || t_[i_] != ':') return fail("expected ':' after the key");
                i_++;
                ws();
                Json x;
                if (!value(x)) return false;
                for (auto& [key, _] : v.obj)
                    if (key == k) return fail("duplicate key \"" + k + "\"");
                v.obj.push_back({k, std::move(x)});
                ws();
                if (i_ < t_.size() && t_[i_] == ',') {
                    i_++;
                    continue;
                }
                if (i_ < t_.size() && t_[i_] == '}') {
                    i_++;
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.kind = Json::Arr;
            i_++;
            ws();
            if (i_ < t_.size() && t_[i_] == ']') {
                i_++;
                return true;
            }
            for (;;) {
                ws();
                Json x;
                if (!value(x)) return false;
                v.arr.push_back(std::move(x));
                ws();
                if (i_ < t_.size() && t_[i_] == ',') {
                    i_++;
                    continue;
                }
                if (i_ < t_.size() && t_[i_] == ']') {
                    i_++;
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            v.kind = Json::Str;
            return string(v.s);
        }
        if (lit("true") || lit("false")) {
            v.kind = Json::Bool;
            return true;
        }
        if (lit("null")) return true;
        if (c == '-' || (c >= '0' && c <= '9')) {
            v.kind = Json::Num;
            size_t b = i_;
            if (t_[i_] == '-') i_++;
            while (i_ < t_.size() && (std::isdigit((unsigned char)t_[i_]) || t_[i_] == '.' || t_[i_] == 'e' ||
                                      t_[i_] == 'E' || t_[i_] == '+' || t_[i_] == '-'))
                i_++;
            v.s = t_.substr(b, i_ - b);
            return true;
        }
        return fail(std::string("unexpected character '") + c + "'");
    }

    const SourceFile& f_;
    const std::string& t_;
    Diag& d_;
    size_t i_ = 0;
    int depth_ = 0;
};

// ---------------- penulisan ulang nama berkualifikasi ----------------

struct FileImport {
    std::string path;
    Loc loc;
    bool used = false;
};

class Rewriter {
public:
    Rewriter(Diag& d, std::map<std::string, FileImport>& imports) : d_(d), imp_(imports) {}

    void run(Program& p) {
        for (auto& sd : p.structs)
            for (auto& f : sd->fields) type(f.type_expr);
        for (auto& ed : p.enums)
            for (auto& v : ed->variants)
                for (auto& f : v.fields) type(f.type_expr);
        for (auto& c : p.consts) {
            if (c->has_type) type(c->type);
            if (c->value) expr(*c->value);
        }
        for (auto& fn : p.fns) {
            if (fn->has_receiver) {
                bind(fn->receiver.name, fn->receiver.loc);
                type(fn->receiver.type);
            }
            for (auto& prm : fn->params) {
                bind(prm.name, prm.loc);
                type(prm.type);
            }
            for (auto& r : fn->ret_types) type(r);
            if (fn->body) block(*fn->body);
        }
    }

private:
    FileImport* alias(const std::string& n) {
        auto it = imp_.find(n);
        return it == imp_.end() ? nullptr : &it->second;
    }

    // alias.Nama → "<path>.Nama"
    std::string qualify(FileImport& im, const std::string& alias_name, const std::string& name, Loc loc) {
        im.used = true;
        std::string head = name.substr(0, name.find('.'));
        if (!exported(head))
            d_.error(loc, "'" + head + "' is not exported by package '" + alias_name + "'",
                     "only names starting with an upper-case letter are visible outside their package");
        return im.path + "." + name;
    }

    void bind(const std::string& n, Loc loc) {
        if (alias(n))
            d_.error(loc, "'" + n + "' shadows the imported package '" + n + "'",
                     "rename the variable, or import the package under another name: import x \"" +
                         alias(n)->path + "\"");
    }

    void type(TypeExpr& te) {
        if (te.kind == TypeExprKind::Name) {
            size_t dot = te.name.find('.');
            if (dot != std::string::npos && te.name.find('/') == std::string::npos) {
                std::string a = te.name.substr(0, dot);
                if (FileImport* im = alias(a)) te.name = qualify(*im, a, te.name.substr(dot + 1), te.loc);
                else d_.error(te.loc, "unknown package '" + a + "'", "import it first: import \"<module>/" + a + "\"");
            }
        }
        if (te.elem) type(*te.elem);
        for (auto& t : te.fn_params) type(t);
        for (auto& t : te.targs) type(t);
        if (te.fn_ret) type(*te.fn_ret);
    }

    // "a.B" / "a.E.V" dengan a alias import → nama internal; selain itu tidak berubah
    void qualified_str(std::string& s, Loc loc) {
        size_t dot = s.find('.');
        if (dot == std::string::npos || s.find('/') != std::string::npos) return;
        std::string a = s.substr(0, dot);
        if (FileImport* im = alias(a)) s = qualify(*im, a, s.substr(dot + 1), loc);
    }

    void expr(Expr& e) {
        switch (e.kind) {
            case ExprKind::Field:
                if (e.lhs->kind == ExprKind::Name)
                    if (FileImport* im = alias(e.lhs->str)) {
                        // utils.MaxSize / utils.Color (lalu .Red oleh Field luar)
                        std::string a = e.lhs->str;
                        e.kind = ExprKind::Name;
                        e.str = qualify(*im, a, e.str, e.loc);
                        e.lhs.reset();
                        return;
                    }
                break;
            case ExprKind::Call:
                if (e.method_call && !e.args.empty() && e.args[0]->kind == ExprKind::Name)
                    if (FileImport* im = alias(e.args[0]->str)) {
                        // utils.Add(1, 2) diparse sebagai method call pada 'utils'
                        std::string a = e.args[0]->str;
                        e.str = qualify(*im, a, e.str, e.loc);
                        e.method_call = false;
                        e.args.erase(e.args.begin());
                    }
                break;
            case ExprKind::StructLit:
            case ExprKind::Spawn: qualified_str(e.str, e.loc); break;
            case ExprKind::Name:
                if (alias(e.str)) {
                    alias(e.str)->used = true;
                    d_.error(e.loc, "use of package '" + e.str + "' without a selector",
                             "write '" + e.str + ".Name'");
                }
                break;
            case ExprKind::Cast: type(e.cast_type); break;
            case ExprKind::ArrayLit:
            case ExprKind::ChanMake: type(e.array_type); break;
            case ExprKind::BlockLit:
                for (size_t i = 0; i < e.block_params.size(); i++) bind(e.block_params[i], e.block_param_locs[i]);
                break;
            default: break;
        }
        if (e.lhs) expr(*e.lhs);
        if (e.rhs) expr(*e.rhs);
        if (e.end) expr(*e.end);
        for (auto& a : e.args)
            if (a) expr(*a);
        if (e.block) block(*e.block);
    }

    void block(Block& b) {
        for (auto& s : b.stmts)
            if (s) stmt(*s);
    }

    void stmt(Stmt& s) {
        for (size_t i = 0; i < s.names.size(); i++)
            bind(s.names[i], i < s.name_locs.size() ? s.name_locs[i] : s.loc);
        if (s.has_type) type(s.type);
        if (s.e1) expr(*s.e1);
        if (s.e2) expr(*s.e2);
        if (s.e3) expr(*s.e3);
        for (auto& e : s.exprs)
            if (e) expr(*e);
        if (s.body) block(*s.body);
        if (s.else_body) block(*s.else_body);
        for (auto& a : s.arms) {
            if (!a.bind.empty()) bind(a.bind, a.bind_loc);
            for (auto& v : a.values)
                if (v) expr(*v);
            if (a.body) block(*a.body);
        }
        for (auto& a : s.sel) {
            if (!a.bind.empty()) bind(a.bind, a.bind_loc);
            if (a.op) expr(*a.op);
            if (a.body) block(*a.body);
        }
    }

    Diag& d_;
    std::map<std::string, FileImport>& imp_;
};

// ---------------- loader ----------------

class Loader {
public:
    Loader(Diag& d, Program& out, LoadedProgram& lp) : d_(d), out_(out), lp_(lp) {}

    bool load(const std::string& input) {
        std::error_code ec;
        fs::path in = fs::absolute(fs::path(input), ec);
        bool is_dir = fs::is_directory(in, ec);
        if (!is_dir && !fs::is_regular_file(in, ec)) {
            std::fprintf(stderr, "error: cannot read '%s'\n", input.c_str());
            return false;
        }
        fs::path dir = is_dir ? in : in.parent_path();
        fs::path manifest;
        for (fs::path p = dir;; p = p.parent_path()) {
            if (fs::is_regular_file(p / kManifest, ec)) {
                manifest = p / kManifest;
                break;
            }
            if (p == p.parent_path()) break;
        }
        if (manifest.empty()) {
            if (is_dir) {
                std::fprintf(stderr, "error: '%s' is a folder but no %s was found in it or above it\n"
                                     "  help: create %s with { \"module\": \"<name>\" } at the project root\n",
                             input.c_str(), kManifest, kManifest);
                return false;
            }
            return load_single(in);
        }
        if (!read_manifest(manifest)) return false;
        lp_.main_dir = dir.string();
        load_dir(dir, "", "main", Loc{});
        return !d_.has_errors();
    }

private:
    SourceFile* new_file(const fs::path& p, const std::string& pkg) {
        auto f = std::make_unique<SourceFile>();
        f->path = show_path(p);
        f->pkg = pkg;
        f->trusted = is_std(pkg);
        SourceFile* raw = f.get();
        lp_.files.push_back(std::move(f));
        return raw;
    }

    bool read_manifest(const fs::path& p) {
        SourceFile* f = new_file(p, "");
        if (!read_text(p, f->text)) {
            std::fprintf(stderr, "error: cannot read '%s'\n", f->path.c_str());
            return false;
        }
        f->index_lines();
        Json j;
        if (!JsonParser(*f, d_).parse(j)) return false;
        Loc top{1, 1, f};
        if (j.kind != Json::Obj) {
            d_.error(top, "forest.json must be a JSON object", "e.g. { \"module\": \"myapp\" }");
            return false;
        }
        const Json* m = j.get("module");
        if (!m || m->kind != Json::Str) {
            d_.error(m ? m->loc : top, "forest.json needs a \"module\" string", "e.g. { \"module\": \"myapp\" }");
            return false;
        }
        bool ok = !m->s.empty() && m->s.front() != '/' && m->s.back() != '/';
        for (size_t i = 0; ok && i < m->s.size(); i++) {
            char c = m->s[i];
            ok = std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.' || c == '/';
            if (c == '/' && i + 1 < m->s.size() && m->s[i + 1] == '/') ok = false;
        }
        if (!ok) {
            d_.error(m->loc, "invalid module name \"" + m->s + "\"",
                     "use letters, digits, '_', '-', '.' and '/' separators, e.g. \"myapp\" or \"github.com/me/app\"");
            return false;
        }
        root_ = p.parent_path();
        module_ = m->s;
        lp_.module = module_;
        return true;
    }

    Program parse_file(SourceFile* f) {
        f->index_lines();
        std::vector<Token> toks = lex(*f, d_);
        return parse(toks, d_);
    }

    void add_sidecar(const fs::path& tree) {
        std::error_code ec;
        fs::path side = fs::path(tree).replace_extension(".c");
        if (fs::exists(side, ec)) lp_.sidecars.push_back(side.string());
    }

    bool load_single(const fs::path& file) {
        SourceFile* f = new_file(file, "");
        if (!read_text(file, f->text)) {
            std::fprintf(stderr, "error: cannot read '%s'\n", f->path.c_str());
            return false;
        }
        Program p = parse_file(f);
        if (!p.package_name.empty() && p.package_name != "main")
            d_.error(p.package_loc, "a single-file program is 'package main', found 'package " + p.package_name + "'");
        process_imports(p, "");
        merge(p, "");
        add_sidecar(file);
        return !d_.has_errors();
    }

    // Memuat semua *.tree di satu folder sebagai satu package.
    //   pkg = import path ("" untuk package main), name = nama package yang diharapkan
    void load_dir(const fs::path& dir, const std::string& pkg, const std::string& name, Loc from) {
        std::vector<fs::path> files;
        std::error_code ec;
        for (auto& ent : fs::directory_iterator(dir, ec))
            if (ent.is_regular_file(ec) && ent.path().extension() == ".tree") files.push_back(ent.path());
        std::sort(files.begin(), files.end());
        if (files.empty()) {
            if (pkg.empty()) std::fprintf(stderr, "error: no .tree files in '%s'\n", show_path(dir).c_str());
            else d_.error(from, "package \"" + pkg + "\" has no .tree files", "folder: " + show_path(dir));
            if (pkg.empty()) failed_ = true;
            return;
        }
        if (!pkg.empty()) register_pkg_display(pkg);
        for (auto& path : files) {
            SourceFile* f = new_file(path, pkg);
            if (!read_text(path, f->text)) {
                d_.error(from, "cannot read '" + f->path + "'");
                continue;
            }
            Program p = parse_file(f);
            Loc top{1, 1, f};
            if (p.package_name.empty())
                d_.error(top, "missing 'package " + name + "' at the top of the file",
                         "every file in a module starts with its package clause");
            else if (p.package_name != name)
                d_.error(p.package_loc, pkg.empty()
                                            ? "the program folder is 'package main', found 'package " + p.package_name + "'"
                                            : "files in folder '" + last_segment(pkg) + "' must be 'package " + name +
                                                  "', found 'package " + p.package_name + "'",
                         pkg.empty() ? "all files next to the program belong to package main"
                                     : "one folder is one package; the package name is the folder name");
            process_imports(p, pkg);
            merge(p, pkg);
            add_sidecar(path);
        }
    }

    // Import satu file: alias, muat package-nya, tulis ulang 'alias.Nama', lalu tolak yang tak terpakai.
    void process_imports(Program& p, const std::string& pkg) {
        std::map<std::string, FileImport> imports;
        for (auto& im : p.imports) {
            std::string alias = im.alias.empty() ? last_segment(im.path) : im.alias;
            if (!is_ident(alias)) {
                d_.error(im.loc, "cannot derive a package name from \"" + im.path + "\"",
                         "give it a name: import name \"" + im.path + "\"");
                continue;
            }
            if (imports.count(alias)) {
                d_.error(im.loc, "'" + alias + "' is imported twice in this file",
                         "use another name: import other \"" + im.path + "\"");
                continue;
            }
            if (!resolve_import(im, pkg)) continue;
            imports[alias] = FileImport{im.path, im.loc, false};
        }
        Rewriter(d_, imports).run(p);
        for (auto& [alias, im] : imports)
            if (!im.used) d_.error(im.loc, "\"" + im.path + "\" imported and not used",
                                   "remove the import, or use it as '" + alias + ".Name'");
    }

    // Path import → folder; dimuat bila belum (deteksi siklus lewat tumpukan import aktif).
    bool resolve_import(const ImportDecl& im, const std::string& from_pkg) {
        const std::string& path = im.path;
        if (is_std(path)) return resolve_std(im);
        if (module_.empty()) {
            d_.error(im.loc, "imports need a module: no " + std::string(kManifest) + " found",
                     "create forest.json with { \"module\": \"myapp\" } in the project root folder "
                     "(only \"std/...\" can be imported without one)");
            return false;
        }
        if (path == module_) {
            d_.error(im.loc, "cannot import the main package \"" + path + "\"");
            return false;
        }
        bool bad = path.empty() || path.front() == '/' || path.back() == '/' || path.find("//") != std::string::npos ||
                   path.find('\\') != std::string::npos;
        for (size_t s = 0, e; !bad && s <= path.size(); s = e + 1) {
            e = path.find('/', s);
            if (e == std::string::npos) e = path.size();
            std::string seg = path.substr(s, e - s);
            if (seg == "." || seg == "..") bad = true;
        }
        if (bad) {
            d_.error(im.loc, "invalid import path \"" + path + "\"", "write it from the module root, e.g. \"" +
                                                                         module_ + "/utils\"");
            return false;
        }
        if (path.compare(0, module_.size() + 1, module_ + "/") != 0) {
            d_.error(im.loc, "package \"" + path + "\" not found",
                     "packages of this module start with \"" + module_ +
                         "/\"; external packages arrive with the package manager");
            return false;
        }
        std::string rel = path.substr(module_.size() + 1);
        fs::path dir = root_ / fs::path(rel);
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            d_.error(im.loc, "package \"" + path + "\" not found", "no folder " + show_path(dir));
            return false;
        }
        std::string name = last_segment(path);
        if (!is_ident(name)) {
            d_.error(im.loc, "folder name '" + name + "' is not a valid package name",
                     "package folders are named like identifiers (letters, digits, '_')");
            return false;
        }
        auto st = state_.find(path);
        if (st != state_.end() && st->second == 1) {
            std::string chain;
            bool on = false;
            for (auto& s : stack_) {
                if (s == path) on = true;
                if (on) chain += s + " -> ";
            }
            d_.error(im.loc, "import cycle: " + chain + path,
                     "packages cannot import each other; move the shared part into a third package");
            return false;
        }
        (void)from_pkg;
        if (st == state_.end()) {
            state_[path] = 1;
            stack_.push_back(path);
            load_dir(dir, path, name, im.loc);
            stack_.pop_back();
            state_[path] = 2;
        }
        return true;
    }

    // import "std/<pkg>": library standar yang dibawa compiler
    bool resolve_std(const ImportDecl& im) {
        const std::string& path = im.path;
        std::string rel = path.size() > 4 ? path.substr(4) : "";
        std::error_code ec;
        fs::path dir = fs::path(stdlib_dir()) / fs::path(rel);
        if (rel.empty() || rel.find("..") != std::string::npos || stdlib_dir().empty() || !fs::is_directory(dir, ec)) {
            d_.error(im.loc, "standard package \"" + path + "\" not found",
                     stdlib_dir().empty() ? "the stdlib folder was not found next to forest (set FOREST_STDLIB)"
                                          : "available packages are folders in " + show_path(stdlib_dir()));
            return false;
        }
        auto st = state_.find(path);
        if (st != state_.end() && st->second == 1) {
            d_.error(im.loc, "import cycle through \"" + path + "\"");
            return false;
        }
        if (st == state_.end()) {
            state_[path] = 1;
            stack_.push_back(path);
            load_dir(dir, path, last_segment(path), im.loc);
            stack_.pop_back();
            state_[path] = 2;
        }
        return true;
    }

    // Pindahkan deklarasi file ke program gabungan; package non-main → nama internal "<pkg>.Nama".
    void merge(Program& p, const std::string& pkg) {
        auto q = [&](std::string& n) {
            if (!pkg.empty()) n = pkg + "." + n;
        };
        for (auto& sd : p.structs) {
            q(sd->name);
            out_.structs.push_back(std::move(sd));
        }
        for (auto& ed : p.enums) {
            q(ed->name);
            out_.enums.push_back(std::move(ed));
        }
        for (auto& c : p.consts) {
            q(c->name);
            out_.consts.push_back(std::move(c));
        }
        for (auto& fn : p.fns) {
            if (!pkg.empty() && !fn->has_receiver) {
                if (fn->name == "main")
                    d_.error(fn->loc, "'main' can only be declared in package main",
                             "a library package has no entry point; call its functions from main");
                if (fn->is_extern) fn->c_name = fn->name;
                q(fn->name);
            }
            out_.fns.push_back(std::move(fn));
        }
    }

    Diag& d_;
    Program& out_;
    LoadedProgram& lp_;
    fs::path root_;
    std::string module_;
    std::map<std::string, int> state_;  // 1 = sedang dimuat, 2 = selesai
    std::vector<std::string> stack_;
    bool failed_ = false;

public:
    bool failed() const { return failed_; }
};

}  // namespace

void set_stdlib_dir(const std::string& dir) { stdlib_dir() = dir; }

bool load_program(const std::string& input, Diag& diag, Program& out, LoadedProgram& lp) {
    Loader l(diag, out, lp);
    bool ok = l.load(input);
    return ok && !l.failed();
}

}  // namespace tl
