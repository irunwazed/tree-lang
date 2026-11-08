// Pemeriksa semantik tree-lang (M1, sintaks ala Go).
//
// Satu pass per fungsi yang sekaligus:
//   - meresolusi nama dan memeriksa tipe (tanpa konversi implisit),
//   - melacak variabel yang pasti sudah diinisialisasi (definite initialization),
//   - melacak rentang nilai integer, per variabel DAN per jalur field (p.x, a.b.c),
//     dengan abstract interpretation: penyempitan dari kondisi, fixpoint loop dengan
//     widening berambang, dan analisis induksi untuk loop 'for',
//   - MENOLAK setiap operasi yang tidak terbukti aman: overflow, bagi nol, shift di
//     luar batas, cast menyempit,
//   - menolak hasil fungsi yang diabaikan (must-use) dan fungsi yang bisa selesai
//     tanpa return.
// Program yang lolos tahap ini tidak punya panic implisit.

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

#include "range.hpp"
#include "sema.hpp"

namespace tl {

namespace {

// Lokasi nilai integer yang dilacak: variabel + rantai indeks field.
struct Path {
    VarInfo* v = nullptr;
    std::vector<int> f;
    bool operator<(const Path& o) const { return v != o.v ? v < o.v : f < o.f; }
    bool operator==(const Path& o) const { return v == o.v && f == o.f; }
};

// Jalur khusus panjang array: Path{xs, {kLenField}} = len(xs).
constexpr int kLenField = -1;
// Batas atas panjang array tumbuh (runtime menolak reservasi lebih besar).
const Big kMaxLen = Big::of_u64(1ull << 40);

struct Env {
    std::map<Path, Interval> r;  // rentang yang lebih sempit dari tipe (absen = rentang penuh)
    std::set<VarInfo*> init;     // variabel yang pasti sudah diisi
    std::set<VarInfo*> moved;    // variabel yang (mungkin) sudah dipindahkan lewat channel
    // fakta 'idx < len(arr)': tetap benar walau arr di-push (array hanya tumbuh)
    std::set<std::pair<Path, Path>> lt;  // (indeks, jalur array)
    bool dead = false;           // titik ini tidak bisa dicapai
};

bool same_interval(const Interval& a, const Interval& b) {
    return a.lo == b.lo && a.hi == b.hi && a.excl_zero == b.excl_zero;
}

Env join(const Env& a, const Env& b) {
    if (a.dead) return b;
    if (b.dead) return a;
    Env out;
    for (auto& [p, ra] : a.r) {
        auto it = b.r.find(p);
        if (it != b.r.end()) out.r[p] = hull(ra, it->second);
    }
    for (VarInfo* v : a.init)
        if (b.init.count(v)) out.init.insert(v);
    // mungkin-dipindah di salah satu jalur = mungkin-dipindah
    out.moved = a.moved;
    out.moved.insert(b.moved.begin(), b.moved.end());
    for (auto& f : a.lt)
        if (b.lt.count(f)) out.lt.insert(f);
    return out;
}

bool path_under(const Path& k, const Path& p) {
    return k.v == p.v && k.f.size() >= p.f.size() && std::equal(p.f.begin(), p.f.end(), k.f.begin());
}

// Hapus semua pengetahuan di bawah jalur 'p': rentang, panjang array, dan fakta indeks.
void erase_under(Env& env, const Path& p) {
    for (auto it = env.r.begin(); it != env.r.end();) it = path_under(it->first, p) ? env.r.erase(it) : std::next(it);
    for (auto it = env.lt.begin(); it != env.lt.end();)
        it = (path_under(it->first, p) || path_under(it->second, p)) ? env.lt.erase(it) : std::next(it);
}

std::string op_text(Tok t) {
    std::string s = tok_name(t);
    if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') s = s.substr(1, s.size() - 2);
    return s;
}

std::string expr_str(const Expr& e) {
    switch (e.kind) {
        case ExprKind::IntLit: return (e.int_neg ? "-" : "") + std::to_string(e.int_val);
        case ExprKind::FloatLit: {
            std::string s = std::to_string(e.float_val);
            while (s.size() > 1 && s.back() == '0' && s[s.size() - 2] != '.') s.pop_back();
            return s;
        }
        case ExprKind::BoolLit: return e.bool_val ? "true" : "false";
        case ExprKind::StrLit: return "\"" + e.str + "\"";
        case ExprKind::Name: return e.str;
        case ExprKind::Unary: return op_text(e.op) + expr_str(*e.lhs);
        case ExprKind::Binary: {
            auto side = [](const Expr& x) {
                return x.kind == ExprKind::Binary ? "(" + expr_str(x) + ")" : expr_str(x);
            };
            return side(*e.lhs) + " " + op_text(e.op) + " " + side(*e.rhs);
        }
        case ExprKind::Call: {
            std::string s;
            size_t first = 0;
            if (e.method_call && !e.args.empty()) {
                s = expr_str(*e.args[0]) + ".";
                first = 1;
            }
            s += e.str + "(";
            for (size_t i = first; i < e.args.size(); i++) {
                if (i > first) s += ", ";
                s += expr_str(*e.args[i]);
            }
            return s + ")";
        }
        case ExprKind::Field: return expr_str(*e.lhs) + "." + e.str;
        case ExprKind::Index: return expr_str(*e.lhs) + "[" + expr_str(*e.rhs) + "]";
        case ExprKind::ArrayLit: return e.array_type.str() + "{...}";
        case ExprKind::NoneLit: return "none";
        case ExprKind::BlockLit: return "{...}";
        case ExprKind::SliceOf: return expr_str(*e.lhs) + "[..]";
        case ExprKind::StructLit: return e.str + " { ... }";
        case ExprKind::Cast: {
            const char* k = e.cast_kind == CastKind::Plain     ? " as "
                            : e.cast_kind == CastKind::Checked ? " as! "
                            : e.cast_kind == CastKind::Wrap    ? " as% "
                                                               : " as| ";
            return expr_str(*e.lhs) + k + e.cast_type.name;
        }
    }
    return "?";
}

bool is_untyped_lit(const Expr& e) { return e.kind == ExprKind::IntLit || e.kind == ExprKind::FloatLit; }

bool is_comparison(Tok t) {
    return t == Tok::EqEq || t == Tok::NotEq || t == Tok::Lt || t == Tok::Le || t == Tok::Gt || t == Tok::Ge;
}

Tok negate_cmp(Tok t) {
    switch (t) {
        case Tok::Lt: return Tok::Ge;
        case Tok::Le: return Tok::Gt;
        case Tok::Gt: return Tok::Le;
        case Tok::Ge: return Tok::Lt;
        case Tok::EqEq: return Tok::NotEq;
        case Tok::NotEq: return Tok::EqEq;
        default: return t;
    }
}

Tok flip_cmp(Tok t) {
    switch (t) {
        case Tok::Lt: return Tok::Gt;
        case Tok::Le: return Tok::Ge;
        case Tok::Gt: return Tok::Lt;
        case Tok::Ge: return Tok::Le;
        default: return t;
    }
}

Tok compound_to_binary(Tok t) {
    switch (t) {
        case Tok::PlusEq: return Tok::Plus;
        case Tok::MinusEq: return Tok::Minus;
        case Tok::StarEq: return Tok::Star;
        case Tok::SlashEq: return Tok::Slash;
        case Tok::PercentEq: return Tok::Percent;
        default: return Tok::Eof;
    }
}

// Jalur (variabel/field) yang ditunjuk sebuah ekspresi, jika ada. Konstanta tidak dilacak.
bool path_of(const Expr& e, Path& out) {
    if (e.kind == ExprKind::Name) {
        // referensi lokal 'r := &x' tidak dilacak (menjadi alias); parameter '&T' dilacak
        if (!e.var || e.var->is_const || (e.var->is_ref && !e.var->is_param)) return false;
        out.v = e.var;
        out.f.clear();
        return true;
    }
    if (e.kind == ExprKind::Field && e.field_index >= 0) {
        if (!path_of(*e.lhs, out)) return false;
        out.f.push_back(e.field_index);
        return true;
    }
    // len(xs) / len(b.items) untuk array tumbuh / slice
    if (e.kind == ExprKind::Call && e.builtin == Builtin::Len && e.args.size() == 1) {
        const Expr& a = *e.args[0];
        if (a.type && (a.type->kind == TypeKind::Dyn || a.type->kind == TypeKind::Slice) && path_of(a, out)) {
            out.f.push_back(kLenField);
            return true;
        }
    }
    return false;
}

bool is_len_path(const Path& p) { return !p.f.empty() && p.f.back() == kLenField; }
Path len_of(Path p) {
    p.f.push_back(kLenField);
    return p;
}
Path arr_of(Path p) {
    p.f.pop_back();
    return p;
}

// Ekspresi yang menunjuk lokasi memori (variabel, field, elemen).
bool is_place(const Expr& e) {
    if (e.kind == ExprKind::Name) return e.var && !e.var->is_const;
    if (e.kind == ExprKind::Field || e.kind == ExprKind::Index) return is_place(*e.lhs);
    return false;
}

// Variabel pemilik memori sebuah lokasi; melewati referensi lokal ke root-nya.
VarInfo* place_root(const Expr& e, bool* through_ref = nullptr) {
    const Expr* t = &e;
    while (t->kind == ExprKind::Field || t->kind == ExprKind::Index || t->kind == ExprKind::SliceOf ||
           (t->kind == ExprKind::Unary && t->op == Tok::Amp))
        t = t->lhs.get();
    if (t->kind != ExprKind::Name || !t->var) return nullptr;
    if (t->var->is_ref && !t->var->is_param) {
        if (through_ref) *through_ref = true;
        return t->var->ref_root;
    }
    return t->var;
}

void collect_int_literals(const Expr& e, std::vector<Big>& out) {
    if (e.kind == ExprKind::IntLit) out.push_back(Big::of(e.int_neg, e.int_val));
    if (e.kind == ExprKind::Name && e.var && e.var->is_const && e.var->type->is_int())
        out.push_back(Big::of(e.var->const_neg, e.var->const_mag));
    if (e.lhs) collect_int_literals(*e.lhs, out);
    if (e.rhs) collect_int_literals(*e.rhs, out);
    for (auto& a : e.args) collect_int_literals(*a, out);
}

void collect_block_literals(const Block& b, std::vector<Big>& out) {
    for (auto& s : b.stmts) {
        for (const ExprPtr* e : {&s->e1, &s->e2, &s->e3})
            if (*e) collect_int_literals(**e, out);
        for (auto& e : s->exprs) collect_int_literals(*e, out);
        if (s->body) collect_block_literals(*s->body, out);
        if (s->else_body) collect_block_literals(*s->else_body, out);
        for (auto& a : s->arms) collect_block_literals(*a.body, out);
    }
}

// Salinan sintaksis (tanpa anotasi sema) untuk instansiasi template.
BlockPtr clone_block(const Block& b);

ExprPtr clone_expr(const Expr& e) {
    auto c = std::make_unique<Expr>(e.kind, e.loc);
    if (e.type && e.type->is_error()) c->type = e.type;  // node error dari parser
    c->int_val = e.int_val;
    c->int_neg = e.int_neg;
    c->float_val = e.float_val;
    c->bool_val = e.bool_val;
    c->str = e.str;
    c->op = e.op;
    if (e.lhs) c->lhs = clone_expr(*e.lhs);
    if (e.rhs) c->rhs = clone_expr(*e.rhs);
    if (e.end) c->end = clone_expr(*e.end);
    for (auto& a : e.args) c->args.push_back(clone_expr(*a));
    c->field_names = e.field_names;
    c->field_locs = e.field_locs;
    c->method_call = e.method_call;
    c->cast_type = e.cast_type;
    c->cast_kind = e.cast_kind;
    c->array_type = e.array_type;
    c->block_params = e.block_params;
    c->block_param_locs = e.block_param_locs;
    if (e.block) c->block = std::shared_ptr<Block>(clone_block(*e.block).release());
    return c;
}

StmtPtr clone_stmt(const Stmt& s) {
    auto c = std::make_unique<Stmt>(s.kind, s.loc);
    c->is_var = s.is_var;
    c->zero_init = s.zero_init;
    c->names = s.names;
    c->name_locs = s.name_locs;
    c->has_type = s.has_type;
    c->type = s.type;
    if (s.e1) c->e1 = clone_expr(*s.e1);
    if (s.e2) c->e2 = clone_expr(*s.e2);
    if (s.e3) c->e3 = clone_expr(*s.e3);
    c->op = s.op;
    for (auto& e : s.exprs) c->exprs.push_back(clone_expr(*e));
    if (s.body) c->body = clone_block(*s.body);
    if (s.else_body) c->else_body = clone_block(*s.else_body);
    c->if_some = s.if_some;
    c->from_name = s.from_name;
    c->from_loc = s.from_loc;
    for (auto& a : s.arms) {
        MatchArm m;
        m.kind = a.kind;
        m.loc = a.loc;
        m.name = a.name;
        m.bind = a.bind;
        m.bind_loc = a.bind_loc;
        m.bind_ref = a.bind_ref;
        for (auto& v : a.values) m.values.push_back(clone_expr(*v));
        m.body = clone_block(*a.body);
        c->arms.push_back(std::move(m));
    }
    return c;
}

BlockPtr clone_block(const Block& b) {
    auto c = std::make_unique<Block>();
    c->loc = b.loc;
    c->end_loc = b.end_loc;
    for (auto& st : b.stmts) c->stmts.push_back(clone_stmt(*st));
    return c;
}

bool te_is_fn(const TypeExpr& te) { return te.kind == TypeExprKind::Fn && !te.func_ref; }  // func(...) = nilai biasa

// Apakah ekspresi tipe menyebut salah satu parameter tipe?
bool te_mentions(const TypeExpr& te, const std::vector<std::string>& tps) {
    if (te.kind == TypeExprKind::Name)
        return std::find(tps.begin(), tps.end(), te.name) != tps.end();
    if (te.kind == TypeExprKind::Fn) {
        for (auto& p : te.fn_params)
            if (te_mentions(p, tps)) return true;
        return te.fn_ret && te_mentions(*te.fn_ret, tps);
    }
    return te.elem && te_mentions(*te.elem, tps);
}

class Checker {
public:
    Checker(Program& p, Diag& d) : prog_(p), diag_(d) {}

    SemaResult run() {
        SemaResult res;
        register_structs();
        register_enums();
        collect_consts();
        for (auto& sd : prog_.structs) resolve_struct(sd.get());
        // tipe payload varian dibuat sekarang: ukuran enum (payload terbesar) harus diketahui sejak awal
        for (auto& ed : prog_.enums)
            for (auto& v : ed->variants)
                if (v.payload) Types::struct_(v.payload);
        for (auto& ed : prog_.enums)
            for (auto& v : ed->variants)
                if (v.payload)
                    for (auto& f : v.payload->fields)
                        if (f.type && contains_dyn(f.type)) {
                            diag_.error(f.type_expr.loc, "enum payloads cannot own growable arrays yet",
                                        "store the array in a struct passed by '&', or use a fixed array");
                            f.type = Types::error();
                        }
        collect_fns();
        // defer/recover di mana pun → panic membuka frame (bisa di-recover); selain itu panic = fatal
        for (auto& fn : prog_.fns)
            if (fn->body && !in_prelude(fn->loc) && pm_block(*fn->body)) panic_mode_ = true;
        res.panic_mode = panic_mode_;
        for (auto& fn : prog_.fns) check_fn(*fn);
        while (!pending_.empty()) {
            FnDecl* f = pending_.back();
            pending_.pop_back();
            check_fn(*f);
        }
        {
            auto m = fns_.find("main");
            compute_unwind(m == fns_.end() ? nullptr : m->second);
            for (auto& f : prog_.fns)
                if (run_targets_.count(f.get())) compute_unwind_entry(f.get());
        }
        {
            std::vector<FnDecl*> entries;
            auto m = fns_.find("main");
            if (m != fns_.end()) entries.push_back(m->second);
            for (auto& f : prog_.fns)
                if (run_targets_.count(f.get())) entries.push_back(f.get());
            analyze_stack(entries);
        }
        {
            // laporan diurutkan menurut posisi di sumber
            using Item = std::pair<Loc, std::string>;
            auto by_pos = [](const Item& a, const Item& b) {
                return a.first.line != b.first.line ? a.first.line < b.first.line : a.first.col < b.first.col;
            };
            std::vector<Item> u, pz;
            for (auto& [c, t] : unsafe_calls_) u.push_back(t);
            for (auto& f : prog_.fns)
                if (f->is_extern && !trusted(f->loc))
                    u.push_back({f->loc, loc_text(f->loc) + ": unsafe extern fn '" + f->name + "' (C symbol)"});
            for (auto& [c, t] : panics_) pz.push_back(t);
            std::sort(u.begin(), u.end(), by_pos);
            std::sort(pz.begin(), pz.end(), by_pos);
            for (auto& x : u) res.unsafe_report.push_back(x.second);
            for (auto& x : pz) res.panic_report.push_back(x.second);
        }
        auto it = fns_.find("main");
        if (it == fns_.end()) {
            diag_.error({1, 1}, "program has no 'main' function", "add 'fn main() { ... }'");
        } else {
            FnDecl* m = it->second;
            if (!m->params.empty() || m->ret != Types::void_())
                diag_.error(m->loc, "'main' must take no parameters and return nothing");
            res.main_fn = m;
        }
        return res;
    }

private:
    // ---------------- deklarasi ----------------

    VarInfo* new_var(const std::string& name, Loc loc, Type* type, bool is_mutable) {
        prog_.vars.push_back(std::make_unique<VarInfo>());
        VarInfo* v = prog_.vars.back().get();
        v->name = name;
        v->loc = loc;
        v->type = type;
        v->is_mutable = is_mutable;
        v->id = next_var_id_++;
        return v;
    }

    // Elemen array tidak boleh berupa array tumbuh atau slice (harus berukuran tetap).
    Type* resolve_elem(const TypeExpr& te) {
        Type* el = resolve_type(*te.elem);
        if (el->kind == TypeKind::Fn) {
            diag_.error(te.elem->loc, "functions cannot be stored in arrays");
            return Types::error();
        }
        if (el->kind == TypeKind::Ref) {
            diag_.error(te.elem->loc, "references cannot be stored in arrays");
            return Types::error();
        }
        if (el->kind == TypeKind::Slice || contains_dyn(el)) {
            diag_.error(te.elem->loc, "array elements must have a fixed size, found " + type_name(el),
                        "arrays of growable arrays arrive in a later milestone");
            return Types::error();
        }
        return el;
    }

    Type* resolve_type(const TypeExpr& te) {
        switch (te.kind) {
            case TypeExprKind::Fn: {
                std::vector<Type*> ps;
                if (te.func_ref) {
                    // func(A) R: parameter seperti fungsi biasa (boleh &T dan []T)
                    for (auto& pt : te.fn_params) {
                        Type* t = resolve_type(pt);
                        if (t->kind == TypeKind::Fn) {
                            diag_.error(pt.loc, "a func type cannot take block parameters");
                            t = Types::error();
                        }
                        ps.push_back(t);
                    }
                    Type* r = te.fn_ret ? resolve_type(*te.fn_ret) : Types::void_();
                    if (r->kind == TypeKind::Ref || r->kind == TypeKind::Slice || r->kind == TypeKind::Fn) {
                        diag_.error(te.fn_ret->loc, "a function cannot return " + type_name(r));
                        r = Types::error();
                    }
                    return Types::funcref(ps, r);
                }
                for (auto& pt : te.fn_params) {
                    Type* t = resolve_type(pt);
                    if (t->kind == TypeKind::Ref || t->kind == TypeKind::Fn || t->kind == TypeKind::Slice) {
                        diag_.error(pt.loc, "function parameter types cannot be " + type_name(t) + " yet");
                        t = Types::error();
                    }
                    ps.push_back(t);
                }
                Type* r = te.fn_ret ? resolve_type(*te.fn_ret) : Types::void_();
                return Types::fn(ps, r);
            }
            case TypeExprKind::Optional: {
                Type* el = resolve_type(*te.elem);
                if (el->is_error()) return el;
                if (el->kind == TypeKind::Optional || el->kind == TypeKind::Ref || el->kind == TypeKind::Slice ||
                    contains_dyn(el)) {
                    diag_.error(te.loc, "cannot make an optional of " + type_name(el),
                                contains_dyn(el) ? "optionals cannot own growable arrays yet" : "");
                    return Types::error();
                }
                return Types::optional(el);
            }
            case TypeExprKind::Ref: {
                Type* el = resolve_type(*te.elem);
                if (el->kind == TypeKind::Ref || el->kind == TypeKind::Slice) {
                    diag_.error(te.loc, "cannot take a reference to " + type_name(el));
                    return Types::error();
                }
                return el->is_error() ? el : Types::ref(el);
            }
            case TypeExprKind::Dyn: {
                Type* el = resolve_elem(te);
                return el->is_error() ? el : Types::dyn(el);
            }
            case TypeExprKind::Slice: {
                Type* el = resolve_elem(te);
                return el->is_error() ? el : Types::slice(el);
            }
            case TypeExprKind::Array: {
                Type* el = resolve_elem(te);
                uint64_t n = te.size_val;
                if (!te.size_name.empty()) {
                    VarInfo* cv = named(consts_, te.size_name, te.loc);
                    if (!cv || !cv->type->is_int() || cv->const_neg) {
                        diag_.error(te.loc, "array size '" + te.size_name + "' must be a non-negative integer constant");
                        return Types::error();
                    }
                    n = cv->const_mag;
                }
                if (n > (1ull << 32)) {
                    diag_.error(te.loc, "fixed array is too large", "use a growable array '[..]T'");
                    return Types::error();
                }
                return el->is_error() ? el : Types::array(el, n);
            }
            case TypeExprKind::Name: break;
        }
        if (te.name == "Task" && te.targs.empty()) return Types::thread_();
        if (te.name == "Thread") {
            diag_.error(te.loc, "'Thread' was replaced by 'Task' (the result of 'run f(...)')");
            return Types::error();
        }
        if (te.name == "Chan") {
            if (te.targs.size() != 1) {
                diag_.error(te.loc, "'Chan' takes one type argument, e.g. 'Chan[Job]'");
                return Types::error();
            }
            Type* el = resolve_type(te.targs[0]);
            if (el->is_error()) return el;
            if (!chan_elem_ok(el)) {
                diag_.error(te.loc, "channels carry plain data, channels, or owned arrays (moved), not " + type_name(el));
                return Types::error();
            }
            return Types::wrap(TypeKind::Chan, el);
        }
        if (te.name == "Pool" || te.name == "Handle" || te.name == "Link") {
            if (te.targs.size() != 1) {
                diag_.error(te.loc, "'" + te.name + "' takes one type argument, e.g. '" + te.name + "[Node]'");
                return Types::error();
            }
            // Link/Handle hanya menyimpan offset/indeks: tipe target tidak perlu lengkap (Node berisi Link[Node])
            bool lazy = te.name != "Pool";
            bool saved_lazy = lazy_struct_;
            lazy_struct_ = lazy;
            Type* el = resolve_type(te.targs[0]);
            lazy_struct_ = saved_lazy;
            if (el->is_error()) return el;
            if (te.name == "Pool" && (contains_dyn(el) || el->kind == TypeKind::Ref || el->kind == TypeKind::Slice ||
                                      el->kind == TypeKind::Fn)) {
                diag_.error(te.loc, "pool elements must have a fixed size, found " + type_name(el));
                return Types::error();
            }
            TypeKind k = te.name == "Pool" ? TypeKind::Pool : te.name == "Handle" ? TypeKind::Handle : TypeKind::Link;
            return Types::wrap(k, el);
        }
        if (!te.targs.empty()) {
            diag_.error(te.loc, "type '" + te.name + "' does not take type arguments");
            return Types::error();
        }
        for (auto it = tparams_.rbegin(); it != tparams_.rend(); ++it) {
            auto f = it->m.find(te.name);
            if (f != it->m.end()) return f->second;
        }
        if (Type* t = Types::primitive(te.name)) return t;
        if (EnumDecl* en = named(enums_, te.name, te.loc)) return Types::enum_(en);
        if (StructDecl* sd = named(structs_, te.name, te.loc)) {
            if (lazy_struct_ && sd->resolving) return Types::struct_(sd);
            lazy_struct_ = false;
            resolve_struct(sd);
            if (sd->resolving) return Types::error();  // memuat dirinya sendiri (sudah dilaporkan)
            return Types::struct_(sd);
        }
        diag_.error(te.loc, "unknown type '" + te.name + "'");
        return Types::error();
    }

    void resolve_struct(StructDecl* sd) {
        if (sd->resolved) return;
        if (sd->resolving) {
            diag_.error(sd->loc, "struct '" + sd->name + "' contains itself by value",
                        "a value cannot contain itself; links/references arrive in a later milestone");
            return;
        }
        sd->resolving = true;
        std::set<std::string> seen;
        for (auto& f : sd->fields) {
            if (!seen.insert(f.name).second) diag_.error(f.loc, "duplicate field '" + f.name + "'");
            f.type = resolve_type(f.type_expr);
            if (f.type->kind == TypeKind::Slice || f.type->kind == TypeKind::Ref || f.type->kind == TypeKind::Fn) {
                diag_.error(f.type_expr.loc, "struct fields cannot be " +
                                                 std::string(f.type->kind == TypeKind::Ref ? "references" : "slices"),
                            "store the value itself; references only live in parameters and local 'r := &x'");
                f.type = Types::error();
            }
        }
        sd->resolving = false;
        sd->resolved = true;
    }

    // Setiap varian dengan payload mendapat struct sintetis "Enum.Varian" (dipakai untuk field & binding).
    void register_enums() {
        for (auto& ed : prog_.enums) {
            if (Types::primitive(ed->name) || structs_.count(ed->name)) {
                diag_.error(ed->loc, "'" + ed->name + "' is already a type name");
                continue;
            }
            if (!enums_.emplace(ed->name, ed.get()).second) {
                diag_.error(ed->loc, "duplicate enum '" + ed->name + "'");
                continue;
            }
            if (ed->variants.empty()) diag_.error(ed->loc, "enum '" + ed->name + "' has no variants");
            std::set<std::string> seen;
            for (auto& v : ed->variants) {
                if (!seen.insert(v.name).second) diag_.error(v.loc, "duplicate variant '" + v.name + "'");
                if (!v.has_payload) continue;
                auto sd = std::make_unique<StructDecl>();
                sd->name = ed->name + "." + v.name;
                sd->loc = v.loc;
                sd->fields = v.fields;
                v.payload = sd.get();
                prog_.structs.push_back(std::move(sd));
            }
        }
    }

    void register_structs() {
        for (auto& sd : prog_.structs) {
            if (Types::primitive(sd->name)) {
                diag_.error(sd->loc, "'" + sd->name + "' is a built-in type name");
                continue;
            }
            if (!structs_.emplace(sd->name, sd.get()).second)
                diag_.error(sd->loc, "duplicate struct '" + sd->name + "'");
        }
    }

    void collect_consts() {
        for (auto& c : prog_.consts) {
            Type* ann = c->has_type ? resolve_type(c->type) : nullptr;
            Expr& v = *c->value;
            VarInfo* var = nullptr;
            if (v.kind == ExprKind::IntLit || v.kind == ExprKind::FloatLit || v.kind == ExprKind::BoolLit) {
                Type* t = check_expr(v, ann);
                if (ann && t != ann && !t->is_error())
                    diag_.error(v.loc, "constant '" + c->name + "': expected " + type_name(ann) + ", found " +
                                           type_name(t));
                var = new_var(c->name, c->loc, ann ? ann : t, false);
                var->is_const = true;
                var->const_neg = v.int_neg;
                var->const_mag = v.int_val;
                var->const_f = v.float_val;
                var->const_b = v.bool_val;
            } else {
                diag_.error(v.loc, "constant value must be a literal (constant expressions arrive later)");
                var = new_var(c->name, c->loc, Types::error(), false);
                var->is_const = true;
            }
            c->var = var;
            if (!consts_.emplace(c->name, var).second)
                diag_.error(c->loc, "duplicate constant '" + c->name + "'");
        }
    }

    void collect_fns() {
        for (auto& fn : prog_.fns) {
            if (fn->is_extern) {
                resolve_extern(*fn);
                continue;
            }
            bool has_fn_param = false;
            for (auto& prm : fn->params)
                if (te_is_fn(prm.type)) has_fn_param = true;
            if (!fn->type_params.empty() || has_fn_param) {
                // template: diperiksa per instansiasi (tipe konkret + fungsi yang terikat)
                fn->is_template = true;
                if (fn->has_receiver) {
                    diag_.error(fn->loc, "methods cannot be generic or take function parameters yet",
                                "write a free function, e.g. 'fn " + fn->name + "[T](x T, ...)'");
                    continue;
                }
                if (fn->name == "main") diag_.error(fn->loc, "'main' cannot be generic");
                if (fn->is_thread) diag_.error(fn->loc, "thread functions cannot be generic or take function parameters");
                if (!fns_.emplace(fn->name, fn.get()).second)
                    diag_.error(fn->loc, "duplicate function '" + fn->name + "'");
                std::set<std::string> tps;
                for (auto& tp : fn->type_params)
                    if (!tps.insert(tp).second || Types::primitive(tp) || structs_.count(tp) || enums_.count(tp))
                        diag_.error(fn->loc, "invalid or duplicate type parameter '" + tp + "'");
                fn->ret = Types::void_();  // ditetapkan per instansiasi
                continue;
            }
            if (fn->has_receiver) {
                // fn (p Point) area() → parameter pertama adalah receiver
                Type* rt = resolve_type(fn->receiver.type);
                if (rt->kind == TypeKind::Ref) rt = rt->elem;  // fn (b &Bag) add(...)
                if (!rt->is_error() && rt->kind != TypeKind::Struct && rt->kind != TypeKind::Enum) {
                    diag_.error(fn->receiver.type.loc, "methods can only be declared on struct and enum types, not " +
                                                           type_name(rt));
                    continue;
                }
                if (rt->is_error()) continue;
                if (foreign(rt->sd ? rt->sd->loc : rt->ed->loc, fn->loc)) {
                    diag_.error(fn->receiver.type.loc, "cannot declare methods on " + type_name(rt) +
                                                           ", which belongs to another package",
                                "declare a function that takes it as a parameter instead");
                    continue;
                }
                fn->receiver_struct = rt->sd;
                fn->receiver_type_name = type_name(rt);
                if (rt->sd && rt->sd->find_field(fn->name) >= 0) {
                    diag_.error(fn->loc, "type '" + rt->sd->name + "' has both a field and a method named '" +
                                             fn->name + "'");
                    continue;
                }
                if (!methods_[type_key(rt)].emplace(fn->name, fn.get()).second) {
                    diag_.error(fn->loc, "duplicate method '" + type_name(rt) + "." + fn->name + "'");
                    continue;
                }
                fn->params.insert(fn->params.begin(), fn->receiver);
            } else {
                if (fn->name == "print" || fn->name == "abort") {
                    diag_.error(fn->loc, "'" + fn->name + "' is a built-in function");
                    continue;
                }
                if (!fns_.emplace(fn->name, fn.get()).second) {
                    diag_.error(fn->loc, "duplicate function '" + fn->name + "'",
                                "overloading is not supported; give it another name or make it a method");
                    continue;
                }
            }
            resolve_signature(*fn);
            if (fn->is_thread) {
                if (fn->ret != Types::void_()) diag_.error(fn->loc, "a thread function returns nothing",
                                                           "send results back with 'send(value)'");
                for (size_t i = 0; i < fn->params.size(); i++)
                    if (!sendable(fn->param_types[i]))
                        diag_.error(fn->params[i].loc, "thread parameters are copied into the new thread and must be "
                                                       "plain data, found " + type_name(fn->param_types[i]),
                                    "threads share no memory: pass numbers, structs or fixed arrays");
            }
        }
    }

    // Nilai pemilik yang boleh DIPINDAHKAN antar goroutine (region ikut pindah, pengirim kehilangan akses).
    static bool movable(const Type* t) {
        switch (t->kind) {
            case TypeKind::Dyn:
            case TypeKind::Pool: return sendable(t->elem);
            case TypeKind::Struct: {
                bool owns = false;
                for (auto& f : t->sd->fields) {
                    if (!f.type) return false;
                    if (movable(f.type)) owns = true;
                    else if (!sendable(f.type)) return false;
                }
                return owns;
            }
            default: return false;
        }
    }

    static bool chan_elem_ok(const Type* t) { return sendable(t) || movable(t); }

    // Nilai yang boleh menyeberang antar goroutine dengan disalin (tanpa pointer/region).
    static bool sendable(const Type* t) {
        switch (t->kind) {
            case TypeKind::Chan: return true;  // handle channel: disinkronkan runtime
            case TypeKind::FuncRef: return true;  // pointer kode tanpa data
            case TypeKind::Int:
            case TypeKind::Float:
            case TypeKind::Bool:
            case TypeKind::Handle:
            case TypeKind::Link: return true;
            case TypeKind::Array:
            case TypeKind::Optional: return sendable(t->elem);
            case TypeKind::Struct:
                for (auto& f : t->sd->fields)
                    if (!f.type || !sendable(f.type)) return false;
                return true;
            case TypeKind::Enum:
                for (auto& v : t->ed->variants)
                    if (v.payload && !sendable(v.payload->type)) return false;
                return true;
            default: return false;
        }
    }

    // Tipe yang punya representasi C: angka, bool, []T / &T (T datar), &[..]T (untuk runtime).
    static bool c_param_ok(const Type* t) {
        if (t->is_numeric() || t == Types::bool_()) return true;
        if (t->kind == TypeKind::Slice) return sendable(t->elem);
        if (t->kind == TypeKind::Ref)
            return sendable(t->elem) || (t->elem->kind == TypeKind::Dyn && sendable(t->elem->elem));
        return false;
    }

    void resolve_extern(FnDecl& fn) {
        if (!fns_.emplace(fn.name, &fn).second) diag_.error(fn.loc, "duplicate function '" + fn.name + "'");
        // library standar boleh menitipkan func ke C (C hanya mengembalikan pointer yang ia terima)
        bool fref_ok = trusted(fn.loc);
        for (auto& p : fn.params) {
            Type* t = resolve_type(p.type);
            if (!t->is_error() && !c_param_ok(t) && !(fref_ok && t->kind == TypeKind::FuncRef))
                diag_.error(p.type.loc, "type " + type_name(t) + " cannot be passed to C",
                            "use numbers, bool, '[]T' (pointer + length), '&T' (pointer) with plain-data T");
            fn.param_types.push_back(t);
            p.var = new_var(p.name, p.loc, t->kind == TypeKind::Ref ? t->elem : t, false);
            p.var->is_param = true;
            p.var->is_ref = t->kind == TypeKind::Ref;
        }
        if (fn.ret_types.size() > 1) diag_.error(fn.loc, "an extern function returns at most one value");
        fn.ret = Types::void_();
        if (!fn.ret_types.empty()) {
            Type* r = resolve_type(fn.ret_types[0]);
            if (!r->is_error() && !(r->is_numeric() || r == Types::bool_() || (fref_ok && r->kind == TypeKind::FuncRef)))
                diag_.error(fn.ret_types[0].loc, "an extern function can only return a number or bool, not " +
                                                     type_name(r));
            fn.ret = r;
        }
    }

    bool in_prelude(Loc l) const { return l.file && l.file->path == "<prelude>"; }
    // prelude atau library standar: dipercaya, tidak masuk --unsafe-report / --panic-report
    bool trusted(Loc l) const { return in_prelude(l) || (l.file && l.file->trusted); }

    // ---------------- defer / panic / recover ----------------
    bool pm_expr(const Expr& e) {
        if (e.kind == ExprKind::Call && !e.method_call && e.str == "recover" && !fns_.count("recover")) return true;
        if (e.kind == ExprKind::BlockLit && e.block && pm_block(*e.block)) return true;
        for (const Expr* c : {e.lhs.get(), e.rhs.get(), e.end.get()})
            if (c && pm_expr(*c)) return true;
        for (auto& a : e.args)
            if (a && pm_expr(*a)) return true;
        return false;
    }
    bool pm_block(const Block& b) {
        for (auto& st : b.stmts) {
            const Stmt& x = *st;
            if (x.kind == StmtKind::Defer) return true;
            for (const Expr* c : {x.e1.get(), x.e2.get(), x.e3.get()})
                if (c && pm_expr(*c)) return true;
            for (auto& e : x.exprs)
                if (pm_expr(*e)) return true;
            for (const Block* c : {x.body.get(), x.else_body.get()})
                if (c && pm_block(*c)) return true;
            for (auto& a : x.arms)
                if (a.body && pm_block(*a.body)) return true;
            for (auto& a : x.sel)
                if ((a.op && pm_expr(*a.op)) || (a.body && pm_block(*a.body))) return true;
        }
        return false;
    }

    // nilai nol tipe ini tidak bisa dipakai (handle channel/task kosong)
    static bool has_handle(const Type* t) {
        switch (t->kind) {
            case TypeKind::Chan:
            case TypeKind::FuncRef:
            case TypeKind::Thread: return true;
            case TypeKind::Array:
            case TypeKind::Optional:
            case TypeKind::Dyn: return has_handle(t->elem);
            case TypeKind::Tuple:
                for (auto* e : t->elems)
                    if (has_handle(e)) return true;
                return false;
            case TypeKind::Struct:
                for (auto& f : t->sd->fields)
                    if (f.type && has_handle(f.type)) return true;
                return false;
            default: return false;
        }
    }

    // tipe yang punya nilai nol yang bisa dipakai (func tidak: tidak ada fungsi 'kosong')
    static bool has_zero(const Type* t) {
        switch (t->kind) {
            case TypeKind::FuncRef: return false;
            case TypeKind::Array: return has_zero(t->elem);
            case TypeKind::Struct:
                for (auto& f : t->sd->fields)
                    if (f.type && !has_zero(f.type)) return false;
                return true;
            case TypeKind::Tuple:
                for (auto* e : t->elems)
                    if (!has_zero(e)) return false;
                return true;
            default: return true;  // Optional (none), Dyn (kosong), angka, ...
        }
    }

    bool in_defer_body() const { return in_defer_ && block_ctx_.size() == defer_block_base_; }

    // defer f(args)  /  defer { ... }: dijalankan LIFO saat fungsi keluar (return, panic, return-from)
    void check_defer(Stmt& s) {
        bool ok = true;
        if (!fn_ || !block_ctx_.empty()) {
            diag_.error(s.loc, in_defer_ ? "'defer' cannot be nested inside another defer"
                                         : "'defer' is not allowed inside a block argument",
                        "defer belongs to a function; put it in the function body");
            ok = false;
        } else if (in_defer_) {
            diag_.error(s.loc, "'defer' cannot be nested inside another defer");
            ok = false;
        } else if (!loops_.empty()) {
            diag_.error(s.loc, "'defer' inside a loop would pile up until the function returns",
                        "move the loop body into its own function and defer there");
            ok = false;
        }
        if (s.e1) {
            Expr& c = *s.e1;
            Type* t = check_expr(c, nullptr);
            if (!t->is_error() && t != Types::void_() && contains_dyn(t))
                diag_.error(c.loc, "a deferred call cannot return an owned value (it would be lost)");
            if (c.kind == ExprKind::Call && (c.builtin == Builtin::Panic || c.builtin == Builtin::Recover ||
                                            c.builtin == Builtin::Abort)) {
                diag_.error(c.loc, "'defer " + c.str + "(...)' is not supported", "write 'defer { ... }'");
                return;
            }
            // argumen dievaluasi SEKARANG (seperti Go): disimpan di slot tersembunyi
            for (size_t i = 0; i < c.args.size(); i++) {
                Expr& a = *c.args[i];
                if (!a.type || a.type->is_error()) continue;
                switch (a.kind) {
                    case ExprKind::IntLit:
                    case ExprKind::FloatLit:
                    case ExprKind::BoolLit:
                    case ExprKind::StrLit:
                    case ExprKind::NoneLit: continue;  // konstanta: sama saja kapan dievaluasi
                    default: break;
                }
                if (a.kind == ExprKind::BlockLit) {
                    diag_.error(a.loc, "a deferred call cannot take a block argument", "write 'defer { ... }'");
                    continue;
                }
                if (a.borrow || (a.kind == ExprKind::Unary && a.op == Tok::Amp)) {
                    // referensi: variabel harus masih hidup saat fungsi keluar
                    VarInfo* r = syntactic_root(a);
                    if (r && !outer_var(r))
                        diag_.error(a.loc, "'" + r->name + "' lives in an inner block and is gone when the deferred call runs",
                                    "declare it at the top of the function, or use 'defer { ... }'");
                    continue;
                }
                Type* at = a.wrap_opt ? Types::optional(a.type) : a.type;
                if (contains_dyn(at) || at->kind == TypeKind::Slice || at->kind == TypeKind::Ref) {
                    diag_.error(a.loc, "a deferred call cannot capture " + type_name(at) + " by value",
                                "use 'defer { ... }', which reads the variable when it runs");
                    continue;
                }
                VarInfo* v = new_var("$defer" + std::to_string(next_var_id_), a.loc, at, false);
                auto nm = std::make_unique<Expr>(ExprKind::Name, a.loc);
                nm->str = v->name;
                nm->var = v;
                nm->type = at;
                s.defer_slots.push_back(v);
                s.defer_args.push_back(std::move(c.args[i]));
                c.args[i] = std::move(nm);
            }
        } else if (s.body) {
            Env saved = env_;
            // dijalankan di titik keluar yang tidak diketahui: rentang & fakta dilupakan
            env_.r.clear();
            env_.lt.clear();
            env_.dead = false;
            int first_id = next_var_id_;
            bool prev = in_defer_;
            in_defer_ = true;
            defer_scope_base_ = scopes_.size();
            defer_block_base_ = block_ctx_.size();
            check_block(*s.body);
            in_defer_ = prev;
            for (VarInfo* v : env_.moved)
                if (!saved.moved.count(v) && v->id < first_id)
                    diag_.error(s.loc, "'" + v->name + "' cannot be moved inside defer");
            std::set<VarInfo*> mods;
            collect_assigned(*s.body, mods);
            for (VarInfo* v : mods)
                if (v->id < first_id && v->type && contains_dyn(v->type))
                    diag_.error(s.loc, "defer cannot change '" + v->name + "': it owns memory that is freed after the defers run",
                                "a defer may read owning variables, and change plain values");
            env_ = saved;
        }
        if (ok && fn_) {
            s.defer_id = (int)fn_->defers.size();
            fn_->defers.push_back(&s);
        }
    }

    // parameter atau variabel di scope terluar fungsi (hidup sampai fungsi keluar)
    bool outer_var(VarInfo* v) {
        for (size_t i = 0; i < scopes_.size() && i < 2; i++)
            for (auto& [n, x] : scopes_[i])
                if (x == v) return true;
        return v->is_param;
    }

    Type* check_panic(Expr& e) {
        e.builtin = Builtin::Panic;
        if (!trusted(e.loc)) panics_[&e] = {e.loc, loc_text(e.loc) + ": panic(...)"};
        if (e.args.size() != 1) {
            diag_.error(e.loc, "'panic' takes one message");
            for (auto& a : e.args) check_expr(*a, nullptr);
            return Types::void_();
        }
        Type* t = check_expr(*e.args[0], nullptr);
        bool ok = t == Types::str_lit() || t->is_error() ||
                  ((t->kind == TypeKind::Slice || t->kind == TypeKind::Dyn || t->kind == TypeKind::Array) &&
                   t->elem == Types::int_(8, false));
        if (!ok)
            diag_.error(e.args[0]->loc, "'panic' takes a string message, found " + type_name(t),
                        "e.g. panic(\"bad input\") or 'msg := str(\"bad id \", id)' then panic(msg)");
        return Types::void_();
    }

    Type* check_recover(Expr& e) {
        e.builtin = Builtin::Recover;
        if (!e.args.empty()) diag_.error(e.loc, "'recover' takes no arguments");
        if (!in_defer_body()) {
            diag_.error(e.loc, "'recover' only works directly inside 'defer { ... }'",
                        "write 'defer { if some msg := recover() { ... } }'");
        } else if (fn_ && fn_->ret && has_handle(fn_->ret)) {
            diag_.error(e.loc, "a function that recovers returns its zero value, but " + type_name(fn_->ret) +
                                   " has no usable zero value",
                        "return a struct or optional instead of a channel/task handle");
        }
        // 'if some msg := recover()' → [..]u8 milik msg; sebagai statement: panic dihentikan, pesan dibuang
        if (owned_recv_ok_ == &e) return Types::optional(Types::dyn(Types::int_(8, false)));
        return Types::void_();
    }

    // ---------------- package ----------------
    // Nama internal package non-main: "<importpath>.Nama". Nama tanpa kualifikasi dicari di package
    // file pemakai, lalu di prelude (package main juga melihat namanya sendiri yang tanpa prefix).
    static const std::string& pkg_of(Loc l) {
        static const std::string main_pkg;
        return l.file ? l.file->pkg : main_pkg;
    }
    static bool exported(const std::string& n) { return !n.empty() && std::isupper((unsigned char)n[0]); }
    // deklarasi di 'decl' berada di package lain dari pemakaian di 'use'
    bool foreign(Loc decl, Loc use) const { return !in_prelude(decl) && pkg_of(decl) != pkg_of(use); }

    template <class T>
    T* named(std::unordered_map<std::string, T*>& m, const std::string& n, Loc use) {
        if (n.find('/') != std::string::npos) {  // sudah berkualifikasi (utils.X ditulis ulang loader)
            auto it = m.find(n);
            return it == m.end() ? nullptr : it->second;
        }
        const std::string& p = pkg_of(use);
        if (!p.empty()) {
            auto it = m.find(p + "." + n);
            if (it != m.end()) return it->second;
        }
        auto it = m.find(n);
        if (it == m.end()) return nullptr;
        if (!p.empty() && !in_prelude(it->second->loc)) return nullptr;  // nama package main tak terlihat
        return it->second;
    }

    // akses field/method milik tipe dari package lain: hanya nama berhuruf besar
    void check_visible(Loc decl, Loc use, const std::string& what, const std::string& name, const std::string& owner) {
        if (exported(name) || !foreign(decl, use)) return;
        diag_.error(use, what + " '" + name + "' of '" + owner + "' is not exported",
                    "only names starting with an upper-case letter are visible outside their package");
    }

    // Tipe parameter & hasil (dipakai juga untuk instansiasi template).
    void resolve_signature(FnDecl& fnd) {
        FnDecl* fn = &fnd;
        {
            std::set<std::string> seen;
            for (auto& p : fn->params) {
                if (!seen.insert(p.name).second) diag_.error(p.loc, "duplicate parameter '" + p.name + "'");
                Type* t = resolve_type(p.type);
                if (t->kind == TypeKind::Fn) {
                    fn->param_types.push_back(t);
                    p.var = new_var(p.name, p.loc, t, false);
                    p.var->is_param = true;
                    continue;
                }
                if (t->kind == TypeKind::Dyn) {
                    diag_.error(p.type.loc, "growable arrays are passed as a read-only slice or by reference",
                                "declare the parameter as '[]" + type_name(t->elem) + "' (read) or '&[.." "]" +
                                    type_name(t->elem) + "' (modify)");
                    t = Types::slice(t->elem);
                } else if (contains_dyn(t)) {
                    diag_.error(p.type.loc, "values of type " + type_name(t) +
                                                " own growable arrays and are passed by reference",
                                "declare the parameter as '&" + type_name(t) + "'");
                    t = Types::ref(t);
                }
                check_stack_size(t, p.type.loc);
                fn->param_types.push_back(t);
                bool is_ref = t->kind == TypeKind::Ref;
                p.var = new_var(p.name, p.loc, is_ref ? t->elem : t, true);  // parameter boleh diubah
                p.var->is_param = true;
                p.var->is_ref = is_ref;
            }
            std::vector<Type*> rets;
            for (auto& te : fn->ret_types) {
                Type* t = resolve_type(te);
                if (t->kind == TypeKind::Slice) {
                    diag_.error(te.loc, "slices cannot be returned (they borrow their array)",
                                "return a growable array '[..]" + type_name(t->elem) + "' instead");
                    t = Types::error();
                } else if (t->kind == TypeKind::Ref || t->kind == TypeKind::Fn) {
                    diag_.error(te.loc, t->kind == TypeKind::Ref ? "references cannot be returned yet"
                                                                 : "functions cannot be returned",
                                "return the value, or let the caller pass '&' to a parameter");
                    t = Types::error();
                }
                check_stack_size(t, te.loc);
                rets.push_back(t);
            }
            if (rets.empty()) fn->ret = Types::void_();
            else if (rets.size() == 1) fn->ret = rets[0];
            else fn->ret = Types::tuple(rets);
        }
    }

    // ---------------- instansiasi template ----------------

    struct TParamScope {
        std::map<std::string, Type*> m;
    };

    // Buat (atau pakai ulang) instansiasi template dengan tipe & fungsi terikat tertentu.
    FnDecl* instantiate(FnDecl* tmpl, const std::vector<Type*>& targs, const std::vector<FnBinding>& binds) {
        std::string key = std::to_string((uintptr_t)tmpl);
        for (Type* t : targs) key += "|" + type_name(t);
        for (auto& b : binds) key += "|" + std::to_string((uintptr_t)b.block) + ":" + std::to_string((uintptr_t)b.fn);
        auto it = inst_memo_.find(key);
        if (it != inst_memo_.end()) return it->second;
        auto f = std::make_unique<FnDecl>();
        f->name = tmpl->name;
        f->loc = tmpl->loc;
        for (auto& prm : tmpl->params) {
            Param c;
            c.name = prm.name;
            c.loc = prm.loc;
            c.type = prm.type;
            f->params.push_back(std::move(c));
        }
        f->ret_types = tmpl->ret_types;
        f->body = clone_block(*tmpl->body);
        f->origin = tmpl;
        f->type_args = targs;
        f->bindings = binds;
        std::string tn;
        for (size_t i = 0; i < targs.size(); i++) tn += (i ? "," : "") + type_name(targs[i]);
        f->inst_name = tmpl->name + "[" + tn + "]#" + std::to_string(prog_.instances.size());
        push_tparams(*f);
        resolve_signature(*f);
        tparams_.pop_back();
        FnDecl* raw = f.get();
        prog_.instances.push_back(std::move(f));
        inst_memo_[key] = raw;
        pending_.push_back(raw);
        return raw;
    }

    void push_tparams(const FnDecl& inst) {
        TParamScope sc;
        if (inst.origin)
            for (size_t i = 0; i < inst.origin->type_params.size() && i < inst.type_args.size(); i++)
                sc.m[inst.origin->type_params[i]] = inst.type_args[i];
        tparams_.push_back(std::move(sc));
    }

    // Satukan ekspresi tipe parameter dengan tipe argumen untuk menemukan parameter tipe.
    void unify(const TypeExpr& te, const Type* at, const std::vector<std::string>& tps, std::map<std::string, Type*>& b) {
        if (!at || at->is_error()) return;
        switch (te.kind) {
            case TypeExprKind::Name:
                if (std::find(tps.begin(), tps.end(), te.name) != tps.end() && !b.count(te.name)) {
                    if (at->kind == TypeKind::StrLit) at = Types::dyn(Types::int_(8, false));
                    b[te.name] = const_cast<Type*>(at);
                }
                return;
            case TypeExprKind::Slice:
            case TypeExprKind::Dyn:
            case TypeExprKind::Array:
                if (at->kind == TypeKind::StrLit) unify(*te.elem, Types::int_(8, false), tps, b);
                else if (at->is_array_like()) unify(*te.elem, at->elem, tps, b);
                return;
            case TypeExprKind::Ref: unify(*te.elem, at->kind == TypeKind::Ref ? at->elem : at, tps, b); return;
            case TypeExprKind::Optional: unify(*te.elem, at->kind == TypeKind::Optional ? at->elem : at, tps, b); return;
            case TypeExprKind::Fn:
                if (at->kind == TypeKind::Fn) {
                    for (size_t i = 0; i < te.fn_params.size() && i < at->elems.size(); i++)
                        unify(te.fn_params[i], at->elems[i], tps, b);
                    if (te.fn_ret && at->elem) unify(*te.fn_ret, at->elem, tps, b);
                }
                return;
        }
    }

    Type* resolve_with(const TypeExpr& te, const std::map<std::string, Type*>& b) {
        TParamScope sc;
        sc.m = b;
        tparams_.push_back(std::move(sc));
        Type* t = resolve_type(te);
        tparams_.pop_back();
        return t;
    }

    static const void* type_key(const Type* t) {
        if (t->kind == TypeKind::Struct) return t->sd;
        if (t->kind == TypeKind::Enum) return t->ed;
        return nullptr;
    }

    // Nilai berukuran tetap hidup di stack native: batasi agar tidak terjadi stack overflow.
    static constexpr uint64_t kMaxStackValue = 64 * 1024;
    void check_stack_size(const Type* t, Loc loc) {
        if (t->kind == TypeKind::Dyn || t->kind == TypeKind::Ref || t->is_error()) return;
        uint64_t n = type_size(t);
        if (n > kMaxStackValue)
            diag_.error(loc, "value of type " + type_name(t) + " (" + std::to_string(n) +
                                 " bytes) is too large for the native stack (max 64 KiB)",
                        "use a growable array '[..]T', which lives in its own data region");
    }

    // ---------------- fungsi & blok ----------------

    void check_fn(FnDecl& fn) {
        if (!fn.ret || fn.is_template || fn.is_extern) return;  // duplikat/builtin, template, atau C
        push_tparams(fn);
        struct PopT {
            std::vector<TParamScope>& v;
            ~PopT() { v.pop_back(); }
        } pop_t{tparams_};
        fn_ = &fn;
        env_ = Env{};
        in_defer_ = false;
        fn.defers.clear();
        loops_.clear();
        scopes_.clear();
        scopes_.emplace_back();
        scope_borrows_.clear();
        borrowed_.clear();
        for (auto& p : fn.params) {
            if (!p.var) continue;
            scopes_.back()[p.name] = p.var;
            env_.init.insert(p.var);
        }
        check_block(*fn.body);
        if (fn.ret != Types::void_() && !env_.dead)
            diag_.error(fn.body->end_loc, "function '" + fn.name + "' may reach its end without returning a value",
                        "add a 'return' on every path");
        scopes_.clear();
        fn_ = nullptr;
    }

    void check_block(Block& b) {
        scopes_.emplace_back();
        scope_borrows_.emplace_back();
        bool warned = false;
        for (auto& s : b.stmts) {
            if (env_.dead && !warned) {
                diag_.warning(s->loc, "unreachable code");
                warned = true;
            }
            check_stmt(*s);
        }
        for (VarInfo* r : scope_borrows_.back()) borrowed_[r]--;
        scope_borrows_.pop_back();
        scopes_.pop_back();
    }

    bool is_borrowed(VarInfo* root) {
        auto it = borrowed_.find(root);
        return it != borrowed_.end() && it->second > 0;
    }

    // Sesuatu mungkin telah mengubah memori root lewat referensi: lupakan pengetahuannya.
    void invalidate_root(VarInfo* root) {
        if (root) erase_under(env_, Path{root, {}});
    }

    void declare(VarInfo* v) { scopes_.back()[v->name] = v; }

    VarInfo* lookup(const std::string& name, Loc use) {
        for (size_t i = scopes_.size(); i-- > 0;) {
            auto f = scopes_[i].find(name);
            if (f != scopes_[i].end()) {
                if (in_defer_ && i >= 2 && i < defer_scope_base_ && name[0] != '$')
                    diag_.error(use, "'" + name + "' lives in an inner block and is gone when the defer runs",
                                "a defer can use parameters and variables declared at the top of the function");
                // berada di luar block yang sedang diperiksa → ditangkap (lewat alamatnya)
                for (auto& bc : block_ctx_)
                    if (i < bc.base) bc.captures.insert(f->second);
                return f->second;
            }
        }
        return named(consts_, name, use);
    }

    // ---------------- rentang ----------------

    Interval path_range(const Env& env, const Path& p, const Type* t) {
        auto it = env.r.find(p);
        if (it != env.r.end()) return it->second;
        if (is_len_path(p)) return Interval::of(Big::of_u64(0), kMaxLen);
        return type_range(t);
    }

    void set_path_range(Env& env, const Path& p, const Type* t, Interval r) {
        Interval tr = type_range(t);
        r = intersect(r, tr);
        if (r.empty()) r = tr;
        if (r.lo == tr.lo && r.hi == tr.hi && !r.excl_zero) env.r.erase(p);
        else env.r[p] = r;
    }

    Interval range_in(const Expr& e, const Env& env) {
        if (e.kind == ExprKind::Name && e.var) {
            if (!e.var->type->is_int()) return type_range(nullptr);
            if (e.var->is_const) return Interval::exact(Big::of(e.var->const_neg, e.var->const_mag));
        }
        Path p;
        const Type* t = e.kind == ExprKind::Name && e.var ? e.var->type : e.type;
        if (t && t->is_int() && path_of(e, p)) return path_range(env, p, t);
        if (!e.type || !e.type->is_int()) return type_range(nullptr);
        return range(e);
    }

    Interval range(const Expr& e) {
        auto it = ranges_.find(&e);
        if (it != ranges_.end()) return it->second;
        return type_range(e.type);
    }

    void set_range(const Expr& e, const Interval& r) { ranges_[&e] = r; }

    // ---------------- loop ----------------

    struct LoopFrame {
        std::vector<Env> breaks, conts;
    };

    // Root variabel dari sebuah lokasi secara sintaksis (sebelum resolusi).
    VarInfo* syntactic_root(const Expr& e) {
        const Expr* t = &e;
        while (t->kind == ExprKind::Field || t->kind == ExprKind::Index || t->kind == ExprKind::SliceOf ||
               (t->kind == ExprKind::Unary && t->op == Tok::Amp))
            t = t->lhs.get();
        if (t->kind != ExprKind::Name) return nullptr;
        VarInfo* v = lookup(t->str, t->loc);
        if (v && v->is_ref && !v->is_param) return v->ref_root;
        return v;
    }

    // Semua variabel yang mungkin berubah di dalam badan loop.
    void collect_assigned(const Block& b, std::set<VarInfo*>& out) {
        for (auto& s : b.stmts) {
            if (s->kind == StmtKind::Assign)
                if (VarInfo* v = syntactic_root(*s->e1)) out.insert(v);
            for (const ExprPtr* e : {&s->e1, &s->e2, &s->e3})
                if (*e) collect_expr_mods(**e, out);
            for (auto& e : s->exprs) collect_expr_mods(*e, out);
            if (s->body) collect_assigned(*s->body, out);
            if (s->else_body) collect_assigned(*s->else_body, out);
            for (auto& a : s->arms) collect_assigned(*a.body, out);
        }
    }

    void collect_expr_mods(const Expr& e, std::set<VarInfo*>& out) {
        if (e.kind == ExprKind::Unary && e.op == Tok::Amp)
            if (VarInfo* v = syntactic_root(*e.lhs)) out.insert(v);
        if (e.kind == ExprKind::Call && e.method_call && !e.args.empty()) {
            VarInfo* recv = syntactic_root(*e.args[0]);
            if (recv && (e.str == "push" || e.str == "append" || e.str == "push_link" || e.str == "put" ||
                         e.str == "add" || e.str == "set" || e.str == "remove"))
                out.insert(recv);
            // method dengan receiver &T
            if (recv && recv->type && type_key(recv->type)) {
                auto ms = methods_.find(type_key(recv->type));
                if (ms != methods_.end()) {
                    auto m = ms->second.find(e.str);
                    if (m != ms->second.end() && !m->second->param_types.empty() &&
                        m->second->param_types[0]->kind == TypeKind::Ref)
                        out.insert(recv);
                }
            }
        }
        // meneruskan referensi yang sudah ada ke parameter &T: f(r)
        if (e.kind == ExprKind::Call)
            for (auto& a : e.args)
                if (a->kind == ExprKind::Name)
                    if (VarInfo* v = lookup(a->str, a->loc); v && v->is_ref) out.insert(v->is_param ? v : v->ref_root);
        if (e.kind == ExprKind::BlockLit && e.block) collect_assigned(*e.block, out);
        if (e.lhs) collect_expr_mods(*e.lhs, out);
        if (e.rhs) collect_expr_mods(*e.rhs, out);
        if (e.end) collect_expr_mods(*e.end, out);
        for (auto& a : e.args) collect_expr_mods(*a, out);
    }

    bool expr_block_modifies(const Expr& e, const Path& tp) {
        if (e.kind == ExprKind::BlockLit && e.block && block_modifies(*e.block, tp)) return true;
        if (e.kind == ExprKind::Unary && e.op == Tok::Amp && syntactic_root(*e.lhs) == tp.v) return true;
        // xs.push_link(v) di dalam ekspresi: panjang xs bertambah di luar bentuk yang dihitung induksi
        if (e.kind == ExprKind::Call && e.method_call && !e.args.empty() && e.str == "push_link" &&
            syntactic_root(*e.args[0]) == tp.v)
            return true;
        for (const Expr* c : {e.lhs.get(), e.rhs.get(), e.end.get()})
            if (c && expr_block_modifies(*c, tp)) return true;
        for (auto& a : e.args)
            if (expr_block_modifies(*a, tp)) return true;
        return false;
    }

    bool stmt_exprs_modify(const Stmt& s, const Path& tp) {
        for (const ExprPtr* e : {&s.e1, &s.e2, &s.e3})
            if (*e && expr_block_modifies(**e, tp)) return true;
        for (auto& e : s.exprs)
            if (expr_block_modifies(*e, tp)) return true;
        return false;
    }

    static const VarInfo* assign_root(const Stmt& s) {
        const Expr* t = s.e1.get();
        while (t && (t->kind == ExprKind::Field || t->kind == ExprKind::Index)) t = t->lhs.get();
        return t && t->kind == ExprKind::Name ? t->var : nullptr;
    }

    // Target induksi: variabel integer Path{v} atau panjang array Path{xs, {kLenField}}.
    static bool is_dyn_op(const Stmt& s, const VarInfo* v) {
        return s.kind == StmtKind::ExprStmt && s.e1->kind == ExprKind::Call && s.e1->method_call &&
               (s.e1->builtin == Builtin::Push || s.e1->builtin == Builtin::Append) && !s.e1->args.empty() &&
               s.e1->args[0]->kind == ExprKind::Name && s.e1->args[0]->var == v;
    }

    bool block_modifies(const Block& b, const Path& tp) {
        for (auto& s : b.stmts) {
            if (s->kind == StmtKind::Assign && assign_root(*s) == tp.v) return true;
            if (stmt_exprs_modify(*s, tp)) return true;
            if (is_len_path(tp) && is_dyn_op(*s, tp.v)) return true;
            if (s->body && block_modifies(*s->body, tp)) return true;
            if (s->else_body && block_modifies(*s->else_body, tp)) return true;
            for (auto& a : s->arms)
                if (block_modifies(*a.body, tp)) return true;
        }
        return false;
    }

    static bool contains_exit(const Stmt& s) {
        switch (s.kind) {
            case StmtKind::Return:
            case StmtKind::Break:
            case StmtKind::Continue: return true;
            case StmtKind::ExprStmt:
                return s.e1->kind == ExprKind::Call &&
                       (s.e1->builtin == Builtin::Abort || s.e1->builtin == Builtin::Panic);
            default: break;
        }
        if (s.body)
            for (auto& x : s.body->stmts)
                if (contains_exit(*x)) return true;
        if (s.else_body)
            for (auto& x : s.else_body->stmts)
                if (contains_exit(*x)) return true;
        for (auto& a : s.arms)
            for (auto& x : a.body->stmts)
                if (contains_exit(*x)) return true;
        return false;
    }

    // Perubahan target dalam satu iterasi badan loop.
    //   cond=true : increment mungkin tidak dijalankan → dihitung [min(0,lo), max(0,hi)]
    //               (sound untuk alur apa pun: if, break, continue, return).
    //   cond=false: increment PASTI dijalankan di setiap iterasi yang selesai → [lo, hi].
    // Statement setelah yang bisa keluar/continue dianggap bersyarat. false = bentuk tidak dikenali.
    bool delta_block(const Block& b, const Path& tp, Interval& d, bool cond) {
        for (auto& s : b.stmts) {
            if (!delta_stmt(*s, tp, d, cond)) return false;
            if (contains_exit(*s)) cond = true;
        }
        return true;
    }

    static void add_delta(Interval& d, const Interval& inc, bool cond) {
        Big zero = Big::of_u64(0);
        d.lo = badd(d.lo, cond ? bmin(inc.lo, zero) : inc.lo);
        d.hi = badd(d.hi, cond ? bmax(inc.hi, zero) : inc.hi);
    }

    bool delta_stmt(const Stmt& s, const Path& tp, Interval& d, bool cond) {
        Big zero = Big::of_u64(0);
        bool len = is_len_path(tp);
        if (stmt_exprs_modify(s, tp)) return false;  // diubah lewat block/'&': jumlahnya tidak diketahui
        switch (s.kind) {
            case StmtKind::Assign: {
                if (assign_root(s) != tp.v) return true;
                if (len || s.e1->kind != ExprKind::Name) return false;
                Interval inc;
                const Expr& rhs = *s.e2;
                const VarInfo* v = tp.v;
                auto is_v = [&](const Expr& x) { return x.kind == ExprKind::Name && x.var == v; };
                if (s.op == Tok::PlusEq) inc = range(rhs);
                else if (s.op == Tok::MinusEq) inc = ineg(range(rhs));
                else if (s.op == Tok::Eq && rhs.kind == ExprKind::Binary && rhs.op == Tok::Plus && is_v(*rhs.lhs))
                    inc = range(*rhs.rhs);
                else if (s.op == Tok::Eq && rhs.kind == ExprKind::Binary && rhs.op == Tok::Plus && is_v(*rhs.rhs))
                    inc = range(*rhs.lhs);
                else if (s.op == Tok::Eq && rhs.kind == ExprKind::Binary && rhs.op == Tok::Minus && is_v(*rhs.lhs))
                    inc = ineg(range(*rhs.rhs));
                else
                    return false;
                add_delta(d, inc, cond);
                return true;
            }
            case StmtKind::ExprStmt:
                if (len && is_dyn_op(s, tp.v)) {
                    auto it = grow_by_.find(s.e1.get());
                    Interval inc = s.e1->builtin == Builtin::Push ? Interval::exact(Big::of_u64(1))
                                   : it != grow_by_.end()         ? it->second
                                                                  : Interval::of(zero, kMaxLen);
                    add_delta(d, inc, cond);
                }
                return true;
            case StmtKind::If:
                if (!delta_block(*s.body, tp, d, true)) return false;
                return !s.else_body || delta_block(*s.else_body, tp, d, true);
            case StmtKind::Match:
                for (auto& a : s.arms)
                    if (!delta_block(*a.body, tp, d, true)) return false;
                return true;
            case StmtKind::Block: return delta_block(*s.body, tp, d, cond);
            case StmtKind::For: {
                Interval inner = Interval::exact(zero);
                if (!delta_block(*s.body, tp, inner, true)) return false;
                if (inner.lo.is_zero() && inner.hi.is_zero()) return true;
                auto it = trip_.find(&s);
                if (it == trip_.end() || !it->second.is_fin()) return false;
                d.lo = badd(d.lo, bmul(it->second, inner.lo));
                d.hi = badd(d.hi, bmul(it->second, inner.hi));
                return true;
            }
            case StmtKind::While:
            case StmtKind::Loop: return !block_modifies(*s.body, tp);
            default: return true;
        }
    }

    static const Type* target_type(const Path& p) { return is_len_path(p) ? Types::i64() : p.v->type; }

    static Interval widen(const Interval& old, const Interval& now, const Type* t, const std::vector<Big>& thr) {
        Interval tr = type_range(t);
        Interval out = now;
        if (now.lo < old.lo) {
            out.lo = tr.lo;
            for (const Big& b : thr)
                if (b <= now.lo && tr.lo <= b && out.lo < b) out.lo = b;
        }
        if (old.hi < now.hi) {
            out.hi = tr.hi;
            for (const Big& b : thr)
                if (now.hi <= b && b <= tr.hi && b < out.hi) out.hi = b;
        }
        out.excl_zero = false;
        return out;
    }

    // Cari invarian rentang di kepala loop.
    //   run_body: periksa badan loop dengan env_ = kepala; kembalikan env di back-edge
    //             (akhir badan + semua 'continue').
    //   trip:     batas jumlah iterasi (hanya 'for'); mengaktifkan analisis induksi.
    // Hasil H dijamin invarian: setiap jalur back-edge, diperiksa di bawah H, tetap di
    // dalam H (post-fixpoint), dan akumulator induksi dibatasi oleh v0 + trip * delta.
    Env solve_loop(const Env& entry, const Block& body, const std::vector<Expr*>& extra,
                   const std::function<Env()>& run_body, const Big& trip, bool is_for) {
        std::set<VarInfo*> assigned;
        collect_assigned(body, assigned);
        if (assigned.empty()) return entry;

        std::vector<Big> thr = {Big::of_u64(0)};
        std::vector<Big> lits;
        collect_block_literals(body, lits);
        for (Expr* e : extra)
            if (e) collect_int_literals(*e, lits);
        for (const Big& b : lits) {
            thr.push_back(b);
            thr.push_back(badd(b, Big::of_u64(1)));
            thr.push_back(bsub(b, Big::of_u64(1)));
        }

        std::set<Path> induct;
        if (is_for && trip.is_fin())
            for (VarInfo* v : assigned) {
                if (!entry.init.count(v)) continue;
                if (v->type->is_int()) induct.insert(Path{v, {}});
                else if (v->type->kind == TypeKind::Dyn) induct.insert(Path{v, {kLenField}});
            }

        Env h = entry;
        bool stable = false;
        diag_.push_mute();
        for (int iter = 0; iter < 16 && !stable; iter++) {
            env_ = h;
            Env be = run_body();
            Env next = join(h, be);
            next.init = h.init;
            next.dead = h.dead;
            // akumulator induksi: batas dari hitungan iterasi, bukan dari back-edge
            for (auto it = induct.begin(); it != induct.end();) {
                const Path& p = *it;
                const Type* pt = target_type(p);
                Interval d = Interval::exact(Big::of_u64(0));
                Interval v0 = path_range(entry, p, pt);
                bool ok = delta_block(body, p, d, true);
                Interval b = v0;
                if (ok) {
                    b = Interval::of(badd(v0.lo, bmul(trip, d.lo)), badd(v0.hi, bmul(trip, d.hi)));
                    ok = is_len_path(p) ? !kMaxLen.is_negative() && b.hi <= kMaxLen : fits(b, pt);
                }
                if (!ok) {
                    it = induct.erase(it);
                    continue;
                }
                Interval keep = hull(path_range(h, p, pt), b);
                if (is_len_path(p)) {
                    next.r.erase(p);
                } else {
                    erase_under(next, p);
                }
                set_path_range(next, p, pt, keep);
                ++it;
            }
            next.moved = h.moved;
            next.moved.insert(be.moved.begin(), be.moved.end());
            stable = next.r.size() == h.r.size() && next.lt == h.lt && next.moved == h.moved;
            for (auto& [p, r] : next.r) {
                auto old = h.r.find(p);
                if (old == h.r.end() || !same_interval(old->second, r)) {
                    stable = false;
                    if (iter >= 2 && old != h.r.end() && !induct.count(p)) {
                        const Type* t = p.f.empty() ? p.v->type : nullptr;
                        if (!t) t = is_len_path(p) ? Types::i64() : field_type(p);
                        r = widen(old->second, r, t, thr);
                        if (is_len_path(p)) r = intersect(r, Interval::of(Big::of_u64(0), kMaxLen));
                    }
                }
            }
            if (!stable) h = next;
        }
        diag_.pop_mute();
        if (!stable) {
            // tidak konvergen: rentang penuh untuk semua yang di-assign (selalu invarian)
            h = entry;
            for (VarInfo* v : assigned) erase_under(h, Path{v, {}});
            induct.clear();
        }
        last_induct_ = induct;
        return h;
    }

    const Type* field_type(const Path& p) {
        const Type* t = p.v->type;
        for (int i : p.f) {
            if (!t || t->kind != TypeKind::Struct || i < 0 || i >= (int)t->sd->fields.size()) return nullptr;
            t = t->sd->fields[i].type;
        }
        return t;
    }

    Env end_of_body(LoopFrame& f) {
        Env be = env_;
        for (Env& c : f.conts) be = join(be, c);
        return be;
    }

    void check_while(Stmt& s) {
        Env entry = env_;
        auto run = [&]() {
            check_cond(*s.e1);
            env_ = refine(env_, *s.e1, true);
            loops_.emplace_back();
            check_block(*s.body);
            Env be = end_of_body(loops_.back());
            loops_.pop_back();
            return be;
        };
        Env h = solve_loop(entry, *s.body, {s.e1.get()}, run, Big::pos_inf(), false);
        env_ = h;
        check_cond(*s.e1);
        env_ = refine(h, *s.e1, true);
        loops_.emplace_back();
        check_block(*s.body);
        std::vector<Env> brks = std::move(loops_.back().breaks);
        loops_.pop_back();
        Env exit = refine(h, *s.e1, false);
        for (Env& b : brks) exit = join(exit, b);
        env_ = exit;
    }

    void check_loop(Stmt& s) {
        Env entry = env_;
        auto run = [&]() {
            loops_.emplace_back();
            check_block(*s.body);
            Env be = end_of_body(loops_.back());
            loops_.pop_back();
            return be;
        };
        Env h = solve_loop(entry, *s.body, {}, run, Big::pos_inf(), false);
        env_ = h;
        loops_.emplace_back();
        check_block(*s.body);
        std::vector<Env> brks = std::move(loops_.back().breaks);
        loops_.pop_back();
        Env exit = h;
        exit.dead = true;
        for (Env& b : brks) exit = join(exit, b);
        env_ = exit;
    }

    void check_for(Stmt& s) {
        if (!s.e1 && s.e2) {
            // for x := range ch ?
            diag_.push_mute();
            Type* pre = check_expr(*s.e2, nullptr);
            diag_.pop_mute();
            if (pre->kind == TypeKind::Chan) {
                check_expr(*s.e2, nullptr);
                check_range_chan(s, pre);
                return;
            }
        }
        Type* t = nullptr;
        if (s.e1) {
            bool l = is_untyped_lit(*s.e1), r = is_untyped_lit(*s.e2);
            Type *ta, *tb;
            if (l && !r) {
                tb = check_expr(*s.e2, nullptr);
                ta = check_expr(*s.e1, tb);
            } else {
                ta = check_expr(*s.e1, nullptr);
                tb = check_expr(*s.e2, ta);
            }
            if (!ta->is_error() && !tb->is_error() && ta != tb)
                diag_.error(s.e2->loc, "range bounds have different types: " + type_name(ta) + " and " + type_name(tb));
            t = ta;
        } else {
            t = check_expr(*s.e2, nullptr);
        }
        s.range_array = t->is_array_like();
        Type* elem_t = nullptr;
        std::optional<Path> arr;   // array yang dilintasi / len(arr) sebagai batas
        VarInfo* lock = nullptr;   // root yang dipinjam selama loop (tidak boleh diganti)
        Interval end;
        if (s.range_array) {
            if (s.e1) diag_.error(s.e1->loc, "'a..b' ranges are for integers; iterate an array with 'range xs'");
            Path ap;
            if (!is_place(*s.e2) && s.e2->kind != ExprKind::SliceOf) {
                diag_.error(s.e2->loc, "iterate a variable: store the array in a variable first");
            } else {
                if (path_of(*s.e2, ap)) arr = ap;
                lock = place_root(*s.e2);
            }
            elem_t = t->elem;
            end = t->kind == TypeKind::Array ? Interval::exact(Big::of_u64(t->count))
                  : arr ? path_range(env_, len_of(*arr), Types::i64())
                        : Interval::of(Big::of_u64(0), kMaxLen);
            t = Types::i64();
        } else {
            if (!t->is_int() && !t->is_error()) {
                diag_.error(s.e2->loc, "'for' range must be an integer or an array, found " + type_name(t));
                t = Types::error();
            }
            if (s.names.size() > 1)
                diag_.error(s.name_locs[1], "'for i, x := range' needs an array; for a count use 'for i := range n'");
            end = range(*s.e2);
            Path lp;
            if (path_of(*s.e2, lp) && is_len_path(lp)) arr = arr_of(lp);  // for i := range len(xs)
        }
        Interval start = s.e1 ? range(*s.e1) : Interval::exact(Big::of_u64(0));
        Big trip = bmax(bsub(end.hi, start.lo), Big::of_u64(0));  // batas atas jumlah iterasi
        trip_[&s] = trip;

        Env entry = env_;
        auto run = [&]() {
            LoopFrame f = run_for_body(s, t, start, end, arr ? &*arr : nullptr, elem_t, lock);
            Env be = env_;
            for (Env& c : f.conts) be = join(be, c);
            return be;
        };
        Env h = solve_loop(entry, *s.body, {}, run, trip, true);
        std::set<Path> induct = last_induct_;
        env_ = h;
        LoopFrame f = run_for_body(s, t, start, end, arr ? &*arr : nullptr, elem_t, lock);
        // Keluar normal = setelah tepat k iterasi selesai, k di [tmin, trip]:
        // target = v0 + k * delta_pasti. Contoh: 'for i := range 500 { xs.push(i) }' → len(xs) == 500.
        Env exit = h;
        Big tmin = bmax(bsub(end.lo, start.hi), Big::of_u64(0));
        for (const Path& p : induct) {
            const Type* pt = target_type(p);
            Interval d = Interval::exact(Big::of_u64(0));
            if (!delta_block(*s.body, p, d, false)) continue;
            Interval v0 = path_range(entry, p, pt);
            Interval bound = Interval::of(badd(v0.lo, bmin(bmul(tmin, d.lo), bmul(trip, d.lo))),
                                          badd(v0.hi, bmax(bmul(tmin, d.hi), bmul(trip, d.hi))));
            Interval n = intersect(path_range(h, p, pt), bound);
            if (!n.empty()) set_path_range(exit, p, pt, n);
        }
        for (Env& b : f.breaks) exit = join(exit, b);
        env_ = exit;
    }

    LoopFrame run_for_body(Stmt& s, Type* t, const Interval& start, const Interval& end, const Path* arr,
                           Type* elem_t, VarInfo* lock) {
        s.vars.clear();
        VarInfo* iv = new_var(s.names[0], s.name_locs[0], t, false);
        s.vars.push_back(iv);
        scopes_.emplace_back();
        declare(iv);
        env_.init.insert(iv);
        if (t->is_int()) {
            Interval ir = Interval::of(start.lo, bsub(end.hi, Big::of_u64(1)));
            if (ir.empty()) env_.dead = true;
            else set_path_range(env_, Path{iv, {}}, t, ir);
            if (arr && t == Types::i64()) env_.lt.insert({Path{iv, {}}, *arr});
        }
        if (elem_t && s.names.size() > 1) {
            VarInfo* xv = new_var(s.names[1], s.name_locs[1], elem_t, false);
            s.vars.push_back(xv);
            if (xv->name != "_") declare(xv);
            env_.init.insert(xv);
        }
        loops_.emplace_back();
        // 'for x := range xs': data xs dibaca sepanjang loop, jadi xs tidak boleh diganti di dalamnya
        if (lock) borrowed_[lock]++;
        check_block(*s.body);
        if (lock) borrowed_[lock]--;
        LoopFrame f = std::move(loops_.back());
        loops_.pop_back();
        scopes_.pop_back();
        erase_under(env_, Path{iv, {}});
        for (Env& c : f.conts) erase_under(c, Path{iv, {}});
        for (Env& b : f.breaks) erase_under(b, Path{iv, {}});
        return f;
    }

    // ---------------- statement ----------------

    void check_stmt(Stmt& s) {
        switch (s.kind) {
            case StmtKind::Let: check_let(s); break;
            case StmtKind::Assign: check_assign(s); break;
            case StmtKind::ExprStmt: {
                Type* t = check_expr(*s.e1, nullptr);
                if (s.e1->kind == ExprKind::Call && (s.e1->builtin == Builtin::Abort || s.e1->builtin == Builtin::Panic))
                    env_.dead = true;
                if (t != Types::void_() && !t->is_error())
                    diag_.error(s.e1->loc, "result of type " + type_name(t) + " is not used",
                                "use the value, or discard it explicitly with '_ = " + expr_str(*s.e1) + "'");
                break;
            }
            case StmtKind::Discard: {
                Type* t = check_owned(*s.e1, nullptr);
                if (t == Types::void_()) diag_.error(s.e1->loc, "nothing to discard: expression has no value");
                break;
            }
            case StmtKind::Match: check_match(s); break;
            case StmtKind::Select: check_select(s); break;
            case StmtKind::Defer: check_defer(s); break;
            case StmtKind::If: {
                if (s.if_some) {
                    check_if_some(s);
                    break;
                }
                check_cond(*s.e1);
                Env base = env_;
                env_ = refine(base, *s.e1, true);
                check_block(*s.body);
                Env after_then = env_;
                env_ = refine(base, *s.e1, false);
                if (s.else_body) check_block(*s.else_body);
                env_ = join(after_then, env_);
                break;
            }
            case StmtKind::While: check_while(s); break;
            case StmtKind::Loop: check_loop(s); break;
            case StmtKind::For: check_for(s); break;
            case StmtKind::Break:
            case StmtKind::Continue:
                if (loops_.empty()) {
                    diag_.error(s.loc, std::string("'") + (s.kind == StmtKind::Break ? "break" : "continue") +
                                           "' outside of a loop");
                } else if (s.kind == StmtKind::Break) {
                    loops_.back().breaks.push_back(env_);
                } else {
                    loops_.back().conts.push_back(env_);
                }
                env_.dead = true;
                break;
            case StmtKind::Return:
                if (in_defer_body()) {
                    diag_.error(s.loc, "'return' is not allowed inside defer",
                                "a defer runs while the function is already leaving");
                    env_.dead = true;
                    break;
                }
                check_return(s);
                break;
            case StmtKind::Block: check_block(*s.body); break;
        }
    }

    // Deklarasikan binding payload (salinan, atau referensi '&r' ke dalam subjek) lalu periksa badan.
    void check_with_binding(Block& body, const std::string& name, Loc loc, Type* t, bool by_ref, Expr& subject,
                            VarInfo** out) {
        scopes_.emplace_back();
        scope_borrows_.emplace_back();
        if (!name.empty() && !t->is_error()) {
            VarInfo* v = new_var(name, loc, t, true);
            if (by_ref) {
                VarInfo* root = place_root(subject);
                if (!is_place(subject)) {
                    diag_.error(loc, "'&" + name + "' needs the matched value to be a variable");
                } else if (root) {
                    v->is_ref = true;
                    v->ref_root = root;
                    borrowed_[root]++;  // subjek tidak boleh diganti selama binding hidup
                    scope_borrows_.back().push_back(root);
                }
            }
            env_.init.insert(v);
            if (v->name != "_") declare(v);
            *out = v;
        }
        check_block(body);
        for (VarInfo* r : scope_borrows_.back()) borrowed_[r]--;
        scope_borrows_.pop_back();
        scopes_.pop_back();
    }

    // if some x := opt { ... } else { ... }
    void check_if_some(Stmt& s) {
        Expr* saved_recv = owned_recv_ok_;
        if (s.e1->kind == ExprKind::ChanRecv) owned_recv_ok_ = s.e1.get();  // if some x := <-ch
        if (s.e1->kind == ExprKind::Call && s.e1->str == "recover" && !s.e1->method_call)
            owned_recv_ok_ = s.e1.get();  // if some msg := recover()
        Type* t = check_expr(*s.e1, nullptr);
        owned_recv_ok_ = saved_recv;
        Type* el = Types::error();
        if (t->kind == TypeKind::Optional) el = t->elem;
        else if (!t->is_error())
            diag_.error(s.e1->loc, "'if some' needs an optional value, found " + type_name(t));
        Env base = env_;
        s.vars.clear();
        VarInfo* v = nullptr;
        check_with_binding(*s.body, s.names[0], s.name_locs[0], el, false, *s.e1, &v);
        if (v) s.vars.push_back(v);
        Env after_then = env_;
        env_ = base;
        if (s.else_body) check_block(*s.else_body);
        env_ = join(after_then, env_);
    }

    void check_match(Stmt& s) {
        Type* t = check_expr(*s.e1, nullptr);
        if (t->is_error()) return;
        bool is_enum = t->kind == TypeKind::Enum, is_opt = t->kind == TypeKind::Optional;
        bool is_int = t->is_int(), is_bool = t == Types::bool_();
        if (!is_enum && !is_opt && !is_int && !is_bool) {
            diag_.error(s.e1->loc, "cannot match on a value of type " + type_name(t),
                        "match works on enums, optionals, integers and bool");
            return;
        }
        Env base = env_;
        Env out = base;
        out.dead = true;
        std::set<int> covered;            // varian / some=1 none=0
        std::set<std::string> seen_vals;  // literal integer/bool
        bool wild = false;
        Path sp;
        bool has_path = is_int && path_of(*s.e1, sp);
        for (auto& arm : s.arms) {
            env_ = base;
            arm.var = nullptr;
            if (wild) diag_.warning(arm.loc, "unreachable match arm (after '_')");
            Type* bind_t = nullptr;
            switch (arm.kind) {
                case MatchArm::Wild: wild = true; break;
                case MatchArm::Variant: {
                    if (!is_enum) {
                        diag_.error(arm.loc, "'" + arm.name + "' is a variant pattern, but the value has type " +
                                                 type_name(t));
                        break;
                    }
                    int idx = t->ed->find_variant(arm.name);
                    if (idx < 0) {
                        std::string names;
                        for (auto& v : t->ed->variants) names += (names.empty() ? "" : ", ") + v.name;
                        diag_.error(arm.loc, "enum '" + t->ed->name + "' has no variant '" + arm.name + "'",
                                    "variants: " + names);
                        break;
                    }
                    arm.variant = idx;
                    if (!covered.insert(idx).second) diag_.error(arm.loc, "variant '" + arm.name + "' is matched twice");
                    const VariantDecl& vd = t->ed->variants[idx];
                    if (!arm.bind.empty()) {
                        if (!vd.payload) diag_.error(arm.bind_loc, "variant '" + arm.name + "' has no payload to bind");
                        else bind_t = Types::struct_(vd.payload);
                    }
                    break;
                }
                case MatchArm::Some:
                case MatchArm::None:
                    if (!is_opt) {
                        diag_.error(arm.loc, "'some'/'none' patterns need an optional value, found " + type_name(t));
                        break;
                    }
                    if (!covered.insert(arm.kind == MatchArm::Some ? 1 : 0).second)
                        diag_.error(arm.loc, "pattern matched twice");
                    if (arm.kind == MatchArm::Some && !arm.bind.empty()) bind_t = t->elem;
                    break;
                case MatchArm::Values: {
                    if (!is_int && !is_bool) {
                        diag_.error(arm.loc, "literal patterns need an integer or bool value, found " + type_name(t));
                        break;
                    }
                    Env arm_env = base;
                    arm_env.dead = true;
                    for (auto& v : arm.values) {
                        Type* vt = check_expr(*v, t);
                        bool lit = v->kind == ExprKind::IntLit || v->kind == ExprKind::BoolLit;
                        if (!lit || vt != t) {
                            if (!vt->is_error())
                                diag_.error(v->loc, "match patterns must be literals of type " + type_name(t));
                            continue;
                        }
                        std::string key = expr_str(*v);
                        if (!seen_vals.insert(key).second) diag_.error(v->loc, "value " + key + " is matched twice");
                        Env e = base;
                        if (has_path) refine_path(e, sp, t, Tok::EqEq, range(*v));
                        arm_env = join(arm_env, e);
                    }
                    if (has_path) env_ = arm_env;
                    break;
                }
            }
            check_with_binding(*arm.body, bind_t ? arm.bind : "", arm.bind_loc, bind_t ? bind_t : Types::error(),
                               arm.bind_ref, *s.e1, &arm.var);
            out = join(out, env_);
        }
        // kelengkapan
        if (!wild) {
            std::string missing;
            if (is_enum) {
                for (size_t i = 0; i < t->ed->variants.size(); i++)
                    if (!covered.count((int)i)) missing += (missing.empty() ? "" : ", ") + t->ed->variants[i].name;
            } else if (is_opt) {
                if (!covered.count(1)) missing += "some";
                if (!covered.count(0)) missing += std::string(missing.empty() ? "" : ", ") + "none";
            } else if (is_bool) {
                if (!seen_vals.count("true")) missing += "true";
                if (!seen_vals.count("false")) missing += std::string(missing.empty() ? "" : ", ") + "false";
            } else {
                missing = "other values";
            }
            if (!missing.empty())
                diag_.error(s.loc, "match is not exhaustive: missing " + missing,
                            is_int ? "add a '_ { ... }' arm for the remaining values"
                                   : "add the missing arms, or a '_ { ... }' arm");
        }
        env_ = out;
    }

    void check_cond(Expr& c) {
        Type* t = check_expr(c, Types::bool_());
        if (t != Types::bool_() && !t->is_error())
            diag_.error(c.loc, "condition must be bool, found " + type_name(t),
                        "compare explicitly, e.g. 'x != 0'");
    }

    bool storable(Type* t) {
        return t->kind == TypeKind::Int || t->kind == TypeKind::Float || t->kind == TypeKind::Bool ||
               t->kind == TypeKind::Struct || t->kind == TypeKind::Array || t->kind == TypeKind::Dyn ||
               t->kind == TypeKind::Enum || t->kind == TypeKind::Optional || t->kind == TypeKind::Pool ||
               t->kind == TypeKind::Handle || t->kind == TypeKind::Link || t->kind == TypeKind::Thread ||
               t->kind == TypeKind::Chan || t->kind == TypeKind::FuncRef || t->is_error();
    }

    // Ekspresi di posisi yang menerima kepemilikan (inisialisasi, assignment, return, '_ =').
    // Array tumbuh sementara (hasil fungsi, literal, copy) hanya boleh muncul di sini.
    Type* check_owned(Expr& e, Type* expected) {
        Expr* saved = owned_ok_;
        owned_ok_ = &e;
        Type* t = check_expr(e, expected);
        owned_ok_ = saved;
        // 'ys := xs' akan membuat dua pemilik untuk satu region
        if (is_place(e) && contains_dyn(t)) {
            bool move = returning_ && e.kind == ExprKind::Name && e.var && !e.var->is_param && !e.var->is_ref;
            if (!move) {
                if (t->kind == TypeKind::Dyn)
                    diag_.error(e.loc, "growable arrays are not copied implicitly",
                                "make an explicit copy with 'copy(" + expr_str(e) + ")'");
                else
                    diag_.error(e.loc, "values of type " + type_name(t) + " own growable arrays and are not copied implicitly",
                                e.var && (e.var->is_ref || e.var->is_param)
                                    ? "only local variables can be moved out with 'return'"
                                    : "pass it by reference '&" + expr_str(e) + "', or build a new value");
            }
        }
        return t;
    }

    static bool has_dyn_value(const Type* t) {
        if (t->kind == TypeKind::Tuple) {
            for (Type* el : t->elems)
                if (contains_dyn(el)) return true;
            return false;
        }
        return contains_dyn(t);
    }

    // Rentang field dari literal struct: `let p = P { x: 3, y: 4 }` → p.x ∈ [3,3].
    void record_struct_lit(Env& env, const Path& base, const Expr& lit) {
        if (lit.kind != ExprKind::StructLit || !lit.type || lit.type->kind != TypeKind::Struct) return;
        for (size_t i = 0; i < lit.args.size(); i++) {
            int idx = lit.field_order[i];
            if (idx < 0) continue;
            Path p = base;
            p.f.push_back(idx);
            const Type* ft = lit.type->sd->fields[idx].type;
            if (ft->is_int()) set_path_range(env, p, ft, range(*lit.args[i]));
            else if (ft->kind == TypeKind::Dyn) set_path_range(env, len_of(p), Types::i64(), dyn_len_of(*lit.args[i]));
            else record_struct_lit(env, p, *lit.args[i]);
        }
    }

    // Nilai nol: semua integer (termasuk field struct bersarang) = 0.
    void record_zero(Env& env, const Path& p, const Type* t) {
        if (t->kind == TypeKind::Dyn) {
            set_path_range(env, len_of(p), Types::i64(), Interval::exact(Big::of_u64(0)));
        } else if (t->is_int()) {
            set_path_range(env, p, t, Interval::exact(Big::of_u64(0)));
        } else if (t->kind == TypeKind::Struct) {
            for (size_t i = 0; i < t->sd->fields.size(); i++) {
                Path fp = p;
                fp.f.push_back((int)i);
                record_zero(env, fp, t->sd->fields[i].type);
            }
        }
    }

    void check_let(Stmt& s) {
        s.vars.clear();  // badan loop bisa diperiksa lebih dari sekali (analisis fixpoint)
        if (s.names.size() > 1) {
            if (s.has_type) diag_.error(s.type.loc, "type annotation is not allowed when destructuring");
            if (!s.e1) {
                diag_.error(s.loc, "destructuring requires an initializer");
                return;
            }
            Type* t = check_owned(*s.e1, nullptr);
            std::vector<Type*> elems;
            if (t->kind == TypeKind::Tuple && t->elems.size() == s.names.size()) {
                elems = t->elems;
            } else {
                if (!t->is_error())
                    diag_.error(s.e1->loc, "cannot destructure " + type_name(t) + " into " +
                                               std::to_string(s.names.size()) + " variables");
                elems.assign(s.names.size(), Types::error());
            }
            for (size_t i = 0; i < s.names.size(); i++) {
                VarInfo* v = new_var(s.names[i], s.name_locs[i], elems[i], true);
                s.vars.push_back(v);
                env_.init.insert(v);
            }
            for (VarInfo* v : s.vars)
                if (v->name != "_") declare(v);
            return;
        }
        if (s.e1 && s.e1->kind == ExprKind::Unary && s.e1->op == Tok::Amp) {
            check_local_ref(s);
            return;
        }
        Type* ann = s.has_type ? resolve_type(s.type) : nullptr;
        if (ann && ann->kind == TypeKind::Ref) {
            diag_.error(s.type.loc, "reference variables are declared with 'r := &x'");
            ann = Types::error();
        }
        Type* vt = ann;
        if (s.e1) {
            Type* t = check_owned(*s.e1, ann);
            if (ann && t != ann && !t->is_error() && !ann->is_error())
                diag_.error(s.e1->loc, "expected " + type_name(ann) + ", found " + type_name(t),
                            t->is_numeric() && ann->is_numeric() ? "convert with 'as', e.g. '" +
                                                                       expr_str(*s.e1) + " as " + type_name(ann) + "'"
                                                                 : "");
            if (!vt) vt = t;
        } else if (!ann) {
            vt = Types::error();  // parser sudah melaporkan
        }
        if (!storable(vt)) {
            diag_.error(s.e1 ? s.e1->loc : s.loc, "cannot store a value of type " + type_name(vt) + " in a variable",
                        vt->kind == TypeKind::Tuple ? "destructure it: 'a, b := ...'"
                        : vt->kind == TypeKind::Slice ? "slices are only used as parameters; keep the original array"
                                                      : "");
            vt = Types::error();
        }
        check_stack_size(vt, s.name_locs[0]);
        VarInfo* v = new_var(s.names[0], s.name_locs[0], vt, true);
        s.vars.push_back(v);
        if (s.e1) {
            env_.init.insert(v);
            assign_value(env_, Path{v, {}}, vt, *s.e1, range(*s.e1));
        } else if (s.zero_init) {
            if (!has_zero(vt))
                diag_.error(s.name_locs[0], type_name(vt) + " has no zero value: give '" + s.names[0] + "' a value",
                            "e.g. 'var h = my_handler', or use an optional 'func(...)?'");
            env_.init.insert(v);
            record_zero(env_, Path{v, {}}, vt);
        }
        declare(v);
    }

    // r := &x   —   referensi lokal dengan root yang diinferensi (tanpa anotasi lifetime).
    // Selama r hidup, root dianggap dipinjam: nilainya tidak boleh diganti (agar r tidak menggantung).
    void check_local_ref(Stmt& s) {
        Expr& amp = *s.e1;
        if (s.names.size() != 1 || s.has_type || s.is_var) {
            diag_.error(s.loc, "a reference is declared with 'r := &x'");
            return;
        }
        Type* t = check_lvalue(*amp.lhs, true);
        amp.type = t->is_error() ? t : Types::ref(t);
        VarInfo* root = place_root(*amp.lhs);
        VarInfo* v = new_var(s.names[0], s.name_locs[0], t, true);
        v->is_ref = true;
        v->ref_root = root;
        s.vars.push_back(v);
        env_.init.insert(v);
        if (root) {
            borrowed_[root]++;
            scope_borrows_.back().push_back(root);
        }
        declare(v);
    }

    // Catat rentang setelah 'p = e' (termasuk field dari literal struct atau salinan struct).
    void assign_value(Env& env, const Path& p, const Type* t, const Expr& value, const Interval& r) {
        erase_under(env, p);
        if (t->kind == TypeKind::Dyn) {
            set_path_range(env, len_of(p), Types::i64(), dyn_len_of(value));
        } else if (t->is_int()) {
            set_path_range(env, p, t, r);
            Path src;
            if (path_of(value, src) && !(src == p)) {
                std::vector<std::pair<Path, Path>> copied;
                for (auto& f : env.lt)
                    if (f.first == src) copied.push_back({p, f.second});
                for (auto& f : copied) env.lt.insert(f);
            }
        } else if (t->kind == TypeKind::Struct) {
            record_struct_lit(env, p, value);
            Path src;
            if (path_of(value, src) && !(src == p)) {
                // salinan struct: field-field ikut tersalin
                std::vector<std::pair<Path, Interval>> copied;
                for (auto& [k, kr] : env.r) {
                    if (k.v == src.v && k.f.size() > src.f.size() &&
                        std::equal(src.f.begin(), src.f.end(), k.f.begin())) {
                        Path np = p;
                        np.f.insert(np.f.end(), k.f.begin() + src.f.size(), k.f.end());
                        copied.push_back({np, kr});
                    }
                }
                for (auto& [k, kr] : copied) env.r[k] = kr;
            }
        }
    }

    // Target assignment: nama variabel 'var' atau rantai field darinya.
    Type* check_lvalue(Expr& t, bool need_init) {
        if (t.kind == ExprKind::Name) {
            VarInfo* v = lookup(t.str, t.loc);
            if (!v) {
                diag_.error(t.loc, "unknown variable '" + t.str + "'");
                return t.type = Types::error();
            }
            t.var = v;
            if (v->is_const || !v->is_mutable) {
                diag_.error(t.loc, "cannot assign to constant '" + t.str + "'", "constants cannot change");
            } else if (need_init && !env_.dead && !env_.init.count(v)) {
                diag_.error(t.loc, "use of possibly uninitialized variable '" + t.str + "'");
                env_.init.insert(v);
            } else if (need_init && !env_.dead && env_.moved.count(v)) {
                diag_.error(t.loc, "'" + t.str + "' was moved into a channel; assign the whole variable first");
            }
            return t.type = v->type;
        }
        if (t.kind == ExprKind::Field) {
            Type* lt = check_lvalue(*t.lhs, true);
            if (lt->is_error()) return t.type = Types::error();
            if (lt->kind != TypeKind::Struct) {
                diag_.error(t.loc, "type " + type_name(lt) + " has no fields");
                return t.type = Types::error();
            }
            int idx = lt->sd->find_field(t.str);
            if (idx < 0) {
                diag_.error(t.loc, "struct '" + lt->sd->name + "' has no field '" + t.str + "'");
                return t.type = Types::error();
            }
            t.field_index = idx;
            return t.type = lt->sd->fields[idx].type;
        }
        if (t.kind == ExprKind::Index) {
            Type* at = check_lvalue(*t.lhs, true);
            if (at->is_error()) {
                check_expr(*t.rhs, nullptr);
                return t.type = Types::error();
            }
            if (at->kind == TypeKind::Slice) {
                diag_.error(t.loc, "slices are read-only", "mutable access arrives with references (M3)");
                check_expr(*t.rhs, nullptr);
                return t.type = Types::error();
            }
            return t.type = check_index_part(t, at);
        }
        diag_.error(t.loc, "invalid assignment target", "only variables, fields and array elements can be assigned");
        check_expr(t, nullptr);
        return Types::error();
    }

    void check_assign(Stmt& s) {
        Tok bin = compound_to_binary(s.op);
        bool compound = s.op != Tok::Eq;
        bool is_field = s.e1->kind == ExprKind::Field || s.e1->kind == ExprKind::Index;
        Type* tt = check_lvalue(*s.e1, compound || is_field);
        Interval before = range_in(*s.e1, env_);
        Type* vt = check_owned(*s.e2, tt->is_error() ? nullptr : tt);
        if (tt->is_error() || vt->is_error()) return;
        if (vt != tt) {
            diag_.error(s.e2->loc, "expected " + type_name(tt) + ", found " + type_name(vt));
            return;
        }
        Interval result = range(*s.e2);
        if (compound) {
            if (!tt->is_numeric()) {
                diag_.error(s.loc, "'" + op_text(s.op) + "' requires numeric operands, found " + type_name(tt));
                return;
            }
            if (tt->is_int())
                result = arith(bin, tt, before, range(*s.e2), s.loc,
                               expr_str(*s.e1) + " " + op_text(bin) + " " + expr_str(*s.e2));
        }
        bool through_ref = false;
        VarInfo* root = place_root(*s.e1, &through_ref);
        // array tumbuh (region dibebaskan) dan enum/optional (payload ditafsirkan ulang) tidak boleh
        // diganti selama ada referensi ke dalamnya
        bool replaces = contains_dyn(tt) || tt->kind == TypeKind::Enum || tt->kind == TypeKind::Optional;
        if (replaces && root && is_borrowed(root))
            diag_.error(s.loc, "cannot replace '" + expr_str(*s.e1) + "' while '" + root->name + "' is borrowed",
                        "a reference ('r := &...') or a 'for ... range' loop is still reading it");
        if (s.e1->kind == ExprKind::Name && s.e1->var) env_.moved.erase(s.e1->var);  // nilai baru: hidup lagi
        Path p;
        if (path_of(*s.e1, p)) {
            env_.init.insert(p.v);
            // 'j -= k' / 'j--' dengan k >= 0: j tetap < len(xs) bila sebelumnya begitu
            std::vector<std::pair<Path, Path>> keep;
            if (s.op == Tok::MinusEq && tt->is_int() && !range(*s.e2).lo.is_negative())
                for (auto& f : env_.lt)
                    if (f.first == p) keep.push_back(f);
            assign_value(env_, p, tt, *s.e2, result);
            for (auto& f : keep) env_.lt.insert(f);
        } else if (through_ref) {
            invalidate_root(root);  // tulis lewat referensi lokal: root berubah
        }
    }

    void check_return(Stmt& s) {
        if (!s.from_name.empty()) {
            check_return_from(s);
            return;
        }
        if (!block_ctx_.empty()) {
            check_block_return(s);
            return;
        }
        Type* ret = fn_->ret;
        if (ret == Types::void_()) {
            if (!s.exprs.empty()) {
                diag_.error(s.exprs[0]->loc, "function '" + fn_->name + "' does not return a value");
                for (auto& e : s.exprs) check_expr(*e, nullptr);
            }
            env_.dead = true;
            return;
        }
        std::vector<Type*> want = ret->kind == TypeKind::Tuple ? ret->elems : std::vector<Type*>{ret};
        returning_ = true;  // 'return xs' memindahkan kepemilikan array tumbuh lokal
        if (s.exprs.size() == 1 && want.size() > 1) {
            // meneruskan hasil multi-return: return f(x);
            Type* t = check_owned(*s.exprs[0], nullptr);
            if (t != ret && !t->is_error())
                diag_.error(s.exprs[0]->loc, "expected " + std::to_string(want.size()) + " return values (" +
                                                 type_name(ret) + "), found " + type_name(t));
        } else if (s.exprs.size() != want.size()) {
            diag_.error(s.loc, "expected " + std::to_string(want.size()) + " return value(s), found " +
                                   std::to_string(s.exprs.size()));
            for (auto& e : s.exprs) check_expr(*e, nullptr);
        } else {
            for (size_t i = 0; i < want.size(); i++) {
                Type* t = check_owned(*s.exprs[i], want[i]);
                if (t != want[i] && !t->is_error() && !want[i]->is_error())
                    diag_.error(s.exprs[i]->loc, "expected " + type_name(want[i]) + ", found " + type_name(t));
            }
            // dua kali array yang sama: 'return xs, xs'
            std::set<const VarInfo*> moved;
            for (auto& e : s.exprs)
                if (e->kind == ExprKind::Name && e->var && e->type && e->type->kind == TypeKind::Dyn &&
                    !moved.insert(e->var).second)
                    diag_.error(e->loc, "'" + e->str + "' is returned twice", "return 'copy(" + e->str + ")'");
        }
        returning_ = false;
        env_.dead = true;
    }

    // ---------------- penyempitan rentang dari kondisi ----------------

    void refine_path(Env& env, const Path& p, const Type* t, Tok op, const Interval& o) {
        if (!t || !t->is_int()) return;
        Interval cur = path_range(env, p, t);
        Interval c = type_range(nullptr);
        Big one = Big::of_u64(1);
        switch (op) {
            case Tok::Lt: c.hi = bsub(o.hi, one); break;
            case Tok::Le: c.hi = o.hi; break;
            case Tok::Gt: c.lo = badd(o.lo, one); break;
            case Tok::Ge: c.lo = o.lo; break;
            case Tok::EqEq: c = o; break;
            case Tok::NotEq:
                if (o.is_singleton()) {
                    if (o.lo.is_zero()) c.excl_zero = true;
                    else if (cur.lo == o.lo) c.lo = badd(cur.lo, one);
                    else if (cur.hi == o.lo) c.hi = bsub(cur.hi, one);
                }
                break;
            default: return;
        }
        Interval n = intersect(cur, c);
        if (n.empty()) {
            env.dead = true;
            return;
        }
        set_path_range(env, p, t, n);
    }

    Env refine(Env env, const Expr& c, bool truth) {
        if (env.dead) return env;
        switch (c.kind) {
            case ExprKind::BoolLit:
                if (c.bool_val != truth) env.dead = true;
                return env;
            case ExprKind::Unary:
                if (c.op == Tok::Bang) return refine(env, *c.lhs, !truth);
                return env;
            case ExprKind::Binary: break;
            default: return env;
        }
        if (c.op == Tok::AmpAmp) {
            if (truth) return refine(refine(env, *c.lhs, true), *c.rhs, true);
            return join(refine(env, *c.lhs, false), refine(refine(env, *c.lhs, true), *c.rhs, false));
        }
        if (c.op == Tok::PipePipe) {
            if (truth) return join(refine(env, *c.lhs, true), refine(refine(env, *c.lhs, false), *c.rhs, true));
            return refine(refine(env, *c.lhs, false), *c.rhs, false);
        }
        if (!is_comparison(c.op) || !c.lhs->type || !c.lhs->type->is_int()) return env;
        Tok op = truth ? c.op : negate_cmp(c.op);
        Interval lr = range_in(*c.lhs, env), rr = range_in(*c.rhs, env);
        // perbandingan dua literal/konstanta: bisa langsung diputuskan
        if (lr.is_singleton() && rr.is_singleton() && !lr.excl_zero && !rr.excl_zero) {
            int k = cmp(lr.lo, rr.lo);
            bool val = op == Tok::Lt ? k < 0 : op == Tok::Le ? k <= 0 : op == Tok::Gt ? k > 0
                       : op == Tok::Ge ? k >= 0 : op == Tok::EqEq ? k == 0 : k != 0;
            if (!val) env.dead = true;
            return env;
        }
        Path p, q;
        bool lp = path_of(*c.lhs, p), rp = path_of(*c.rhs, q);
        if (lp) refine_path(env, p, c.lhs->type, op, rr);
        if (!env.dead && rp) refine_path(env, q, c.rhs->type, flip_cmp(op), lr);
        if (!env.dead && lp && rp && c.lhs->type == Types::i64()) {
            // i < len(xs)  /  len(xs) > i
            if (op == Tok::Lt && is_len_path(q) && !is_len_path(p)) env.lt.insert({p, arr_of(q)});
            if (op == Tok::Gt && is_len_path(p) && !is_len_path(q)) env.lt.insert({q, arr_of(p)});
        }
        return env;
    }

    // ---------------- ekspresi ----------------

    Type* check_expr(Expr& e, Type* expected) {
        if (e.type && e.type->is_error()) return e.type;  // node error dari parser
        Expr* owner = owned_ok_;
        if (owner != &e) owned_ok_ = nullptr;  // hanya ekspresi teratas yang boleh menjadi pemilik
        e.wrap_opt = false;
        if (e.kind == ExprKind::NoneLit) {
            owned_ok_ = owner;
            if (expected && expected->kind == TypeKind::Optional) return e.type = expected;
            diag_.error(e.loc, "'none' needs a known optional type here", "e.g. 'var x i64? = none'");
            return e.type = Types::error();
        }
        bool want_opt = expected && expected->kind == TypeKind::Optional;
        Type* t = check_expr_inner(e, want_opt ? expected->elem : expected);
        owned_ok_ = owner;
        e.type = t;
        if (want_opt && t == expected->elem) {
            // nilai T diterima di tempat T?: dibungkus menjadi 'some'
            e.wrap_opt = true;
            t = expected;
        }
        if (owner != &e && !is_place(e) && has_dyn_value(e.type) && !t->is_error())
            diag_.error(e.loc, "a new value owning a growable array must be stored in a variable first",
                        "write 'x := " + expr_str(e) + "' and use 'x'");
        return t;
    }

    Type* check_expr_inner(Expr& e, Type* expected) {
        switch (e.kind) {
            case ExprKind::IntLit: {
                if (expected && expected->is_float()) {
                    e.kind = ExprKind::FloatLit;
                    e.float_val = (e.int_neg ? -1.0 : 1.0) * (double)e.int_val;
                    return expected;
                }
                Type* t = expected && expected->is_int() ? expected : Types::i64();
                Interval v = Interval::exact(Big::of(e.int_neg, e.int_val));
                if (!fits(v, t)) {
                    Interval tr = type_range(t);
                    diag_.error(e.loc, "integer literal " + expr_str(e) + " does not fit in " + type_name(t) +
                                           " (range " + tr.lo.str() + ".." + tr.hi.str() + ")");
                }
                set_range(e, v);
                return t;
            }
            case ExprKind::FloatLit: return expected && expected->is_float() ? expected : Types::float_(64);
            case ExprKind::BoolLit: return Types::bool_();
            case ExprKind::StrLit: {
                Type* u8 = Types::int_(8, false);
                if (expected && expected->kind == TypeKind::Dyn && expected->elem == u8) return expected;
                if (expected && expected->kind == TypeKind::Slice && expected->elem == u8) {
                    e.str_as_slice = true;
                    return expected;
                }
                // tanpa konteks: string disimpan sebagai [..]u8 (salinan), kecuali argumen print/abort
                return owned_ok_ == &e ? Types::dyn(u8) : Types::str_lit();
            }
            case ExprKind::Index: {
                Type* at = check_expr(*e.lhs, nullptr);
                if (at->is_error()) {
                    check_expr(*e.rhs, nullptr);
                    return Types::error();
                }
                return check_index_part(e, at);
            }
            case ExprKind::ArrayLit: return check_array_lit(e);
            case ExprKind::SliceOf: return check_slice_of(e);
            case ExprKind::BlockLit:
                diag_.error(e.loc, "a block '{ ... }' can only be passed to a function parameter");
                return Types::error();
            case ExprKind::Spawn: return check_spawn(e);
            case ExprKind::ChanMake: return check_chan_make(e);
            case ExprKind::ChanSend: return check_chan_send(e);
            case ExprKind::ChanRecv: return check_chan_recv(e);
            case ExprKind::Name: return check_name(e);
            case ExprKind::Unary: return check_unary(e, expected);
            case ExprKind::Binary: return check_binary(e, expected);
            case ExprKind::Call: return check_call(e);
            case ExprKind::Field: {
                EnumDecl* ed0 = e.lhs->kind == ExprKind::Name && !lookup(e.lhs->str, e.lhs->loc)
                                    ? named(enums_, e.lhs->str, e.lhs->loc)
                                    : nullptr;
                if (ed0) {
                    EnumDecl* ed = ed0;
                    int idx = ed->find_variant(e.str);
                    if (idx < 0) {
                        diag_.error(e.loc, "enum '" + ed->name + "' has no variant '" + e.str + "'");
                        return Types::error();
                    }
                    if (ed->variants[idx].payload) {
                        diag_.error(e.loc, "variant '" + e.str + "' has fields",
                                    "write '" + ed->name + "." + e.str + "{...}'");
                        return Types::error();
                    }
                    e.variant = idx;
                    return Types::enum_(ed);
                }
                Type* lt = check_expr(*e.lhs, nullptr);
                if (lt->is_error()) return Types::error();
                if (lt->kind != TypeKind::Struct) {
                    diag_.error(e.loc, "type " + type_name(lt) + " has no fields");
                    return Types::error();
                }
                int idx = lt->sd->find_field(e.str);
                if (idx < 0) {
                    diag_.error(e.loc, "struct '" + lt->sd->name + "' has no field '" + e.str + "'");
                    return Types::error();
                }
                check_visible(lt->sd->loc, e.loc, "field", e.str, lt->sd->name);
                e.field_index = idx;
                Type* ft = lt->sd->fields[idx].type;
                Path p;
                if (ft->is_int() && path_of(e, p)) set_range(e, path_range(env_, p, ft));
                return ft;
            }
            case ExprKind::StructLit: return check_struct_lit(e);
            case ExprKind::Cast: return check_cast(e);
        }
        return Types::error();
    }

    Type* check_name(Expr& e) {
        VarInfo* v = lookup(e.str, e.loc);
        if (!v) {
            if (FnDecl* f = named(fns_, e.str, e.loc)) {
                // referensi ke fungsi bernama: tanpa capture, jadi aman disimpan dan dikirim
                if (f->is_template || f->is_extern || f->is_thread || !f->ret) {
                    diag_.error(e.loc, f->is_template ? "a generic function cannot be used as a value"
                                       : f->is_extern ? "an extern function cannot be used as a value"
                                                      : "'" + e.str + "' cannot be used as a value",
                                "wrap it in a plain function: 'fn my_" + e.str + "(...) { ... }'");
                    return Types::error();
                }
                e.fn = f;
                address_taken_.insert(f);
                return Types::funcref(f->param_types, f->ret);
            }
            {
                diag_.error(e.loc, "unknown name '" + e.str + "'");
            }
            return Types::error();
        }
        e.var = v;
        if (v->type->kind == TypeKind::Fn) {
            diag_.error(e.loc, "function parameter '" + e.str + "' can only be called or passed to another function");
            return Types::error();
        }
        if (env_.moved.count(v) && !env_.dead) {
            diag_.error(e.loc, "'" + e.str + "' was moved into a channel and cannot be used here",
                        "assign it a new value first, or send 'copy(" + e.str + ")' instead");
            env_.moved.erase(v);
        }
        if (!v->is_const && !env_.dead && !env_.init.count(v)) {
            diag_.error(e.loc, "use of possibly uninitialized variable '" + e.str + "'",
                        "initialize it when declaring, or assign it on every path before this use");
            env_.init.insert(v);
        }
        if (v->type->is_int()) set_range(e, range_in(e, env_));
        return v->type;
    }

    void overflow_error(Loc loc, const std::string& text, Type* t, const Interval& r, Tok op) {
        Interval tr = type_range(t);
        std::string alt;
        switch (op) {
            case Tok::Plus: alt = "'+%' (wrap) or '+|' (saturate)"; break;
            case Tok::Minus: alt = "'-%' (wrap) or '-|' (saturate)"; break;
            case Tok::Star: alt = "'*%' (wrap) or '*|' (saturate)"; break;
            default: alt = "a wider type"; break;
        }
        diag_.error(loc,
                    "possible overflow: '" + text + "' may be in " + r.str() + ", outside " + type_name(t) + " range [" +
                        tr.lo.str() + ", " + tr.hi.str() + "]",
                    "prove the operands are small enough with an 'if' check, use a wider type, or use " + alt);
    }

    // Cek operasi integer; laporkan error bila tidak terbukti aman. Mengembalikan rentang hasil.
    Interval arith(Tok op, Type* t, const Interval& a, const Interval& b, Loc loc, const std::string& text) {
        switch (op) {
            case Tok::Plus:
            case Tok::Minus:
            case Tok::Star: {
                Interval r = op == Tok::Plus ? iadd(a, b) : op == Tok::Minus ? isub(a, b) : imul(a, b);
                if (!fits(r, t)) {
                    overflow_error(loc, text, t, r, op);
                    return clamp_to(r, t);
                }
                return r;
            }
            case Tok::PlusWrap:
            case Tok::MinusWrap:
            case Tok::StarWrap: {
                Interval r = op == Tok::PlusWrap ? iadd(a, b) : op == Tok::MinusWrap ? isub(a, b) : imul(a, b);
                return fits(r, t) ? r : type_range(t);
            }
            case Tok::PlusSat:
            case Tok::MinusSat:
            case Tok::StarSat: {
                Interval r = op == Tok::PlusSat ? iadd(a, b) : op == Tok::MinusSat ? isub(a, b) : imul(a, b);
                return clamp_to(r, t);
            }
            case Tok::Slash:
            case Tok::Percent: {
                Interval d = b;
                if (d.contains_zero()) {
                    diag_.error(loc, "possible division by zero: divisor in '" + text + "' may be 0 (range " +
                                         b.str() + ")",
                                "check it first, e.g. 'if d == 0 { return ... }' or 'if d > 0 { ... }'");
                    d.excl_zero = true;
                    d.normalize();
                    if (d.empty()) return type_range(t);
                }
                Interval tr = type_range(t);
                if (t->is_signed && a.contains(tr.lo) && d.contains(Big::of(true, 1))) {
                    diag_.error(loc, "possible overflow: '" + text + "' may compute " + tr.lo.str() + " / -1",
                                "prove the divisor is positive (e.g. 'if d <= 0 { return ... }') or the dividend is "
                                "greater than " + tr.lo.str());
                }
                Interval r = op == Tok::Slash ? idiv(a, d) : irem(a, d);
                Interval ri = intersect(r, tr);
                return ri.empty() ? tr : ri;
            }
            case Tok::Amp: return ibitand(a, b, t);
            case Tok::Pipe:
            case Tok::Caret: return ibitor(a, b, t);
            default: return type_range(t);
        }
    }

    Type* check_unary(Expr& e, Type* expected) {
        switch (e.op) {
            case Tok::Amp:
                diag_.error(e.loc, "'&' is only used for arguments to '&T' parameters and in 'r := &x'");
                check_expr(*e.lhs, nullptr);
                return Types::error();
            case Tok::Bang: {
                Type* t = check_expr(*e.lhs, Types::bool_());
                if (t != Types::bool_() && !t->is_error())
                    diag_.error(e.loc, "'!' requires bool, found " + type_name(t), "for integers use '~' (bitwise not)");
                return Types::bool_();
            }
            case Tok::Minus: {
                Type* t = check_expr(*e.lhs, expected);
                if (t->is_error()) return t;
                if (!t->is_numeric()) {
                    diag_.error(e.loc, "'-' requires a number, found " + type_name(t));
                    return Types::error();
                }
                if (t->is_int()) {
                    if (!t->is_signed) {
                        diag_.error(e.loc, "cannot negate unsigned value of type " + type_name(t),
                                    "use a signed type, or '0 -% x' for wrapping negation");
                        return Types::error();
                    }
                    Interval r = ineg(range(*e.lhs));
                    if (!fits(r, t)) {
                        overflow_error(e.loc, expr_str(e), t, r, Tok::Eof);
                        r = clamp_to(r, t);
                    }
                    set_range(e, r);
                }
                return t;
            }
            case Tok::Tilde: {
                Type* t = check_expr(*e.lhs, expected);
                if (t->is_error()) return t;
                if (!t->is_int()) {
                    diag_.error(e.loc, "'~' requires an integer, found " + type_name(t));
                    return Types::error();
                }
                Interval a = range(*e.lhs);
                Big one = Big::of_u64(1);
                if (t->is_signed) {
                    set_range(e, Interval::of(bsub(bneg(a.hi), one), bsub(bneg(a.lo), one)));
                } else {
                    Big max = type_range(t).hi;
                    set_range(e, Interval::of(bsub(max, a.hi), bsub(max, a.lo)));
                }
                return t;
            }
            default: break;
        }
        return Types::error();
    }

    // Periksa dua operan; literal tanpa tipe mengikuti tipe sisi lainnya.
    Type* check_operands(Expr& e, Type* expected) {
        bool l = is_untyped_lit(*e.lhs), r = is_untyped_lit(*e.rhs);
        Type *lt, *rt;
        if (l && !r) {
            rt = check_expr(*e.rhs, expected);
            lt = check_expr(*e.lhs, rt->is_numeric() ? rt : expected);
        } else {
            lt = check_expr(*e.lhs, expected);
            rt = check_expr(*e.rhs, lt->is_numeric() ? lt : expected);
        }
        if (lt->is_error() || rt->is_error()) return Types::error();
        if (lt != rt) {
            diag_.error(e.loc, "mismatched types in '" + op_text(e.op) + "': " + type_name(lt) + " and " +
                                   type_name(rt),
                        "there are no implicit conversions; convert one side with 'as', e.g. '" +
                            expr_str(*e.rhs) + " as " + type_name(lt) + "'");
            return Types::error();
        }
        return lt;
    }

    Type* check_binary(Expr& e, Type* expected) {
        Tok op = e.op;
        if (op == Tok::AmpAmp || op == Tok::PipePipe) {
            // operan kanan diperiksa di bawah penyempitan dari operan kiri:
            // 'b != 0 && a / b > 1' aman
            Type* lt = check_expr(*e.lhs, Types::bool_());
            Env saved = env_;
            env_ = refine(env_, *e.lhs, op == Tok::AmpAmp);
            Type* rt = check_expr(*e.rhs, Types::bool_());
            env_ = saved;
            for (auto [side, t] : {std::pair{e.lhs.get(), lt}, std::pair{e.rhs.get(), rt}})
                if (t != Types::bool_() && !t->is_error())
                    diag_.error(side->loc, "'" + op_text(op) + "' requires bool operands, found " + type_name(t));
            return Types::bool_();
        }
        if (is_comparison(op)) {
            Type* t = check_operands(e, nullptr);
            if (t->is_error()) return Types::bool_();
            bool eq = op == Tok::EqEq || op == Tok::NotEq;
            if (!(t->is_numeric() || (eq && t == Types::bool_())))
                diag_.error(e.loc, "cannot compare values of type " + type_name(t) + " with '" + op_text(op) + "'");
            return Types::bool_();
        }
        if (op == Tok::Shl || op == Tok::Shr) return check_shift(e, expected);
        if (op == Tok::QQ) {
            Type* lt = check_expr(*e.lhs, nullptr);
            if (lt->is_error()) return lt;
            if (lt->kind != TypeKind::Optional) {
                diag_.error(e.lhs->loc, "'??' needs an optional on the left, found " + type_name(lt));
                check_expr(*e.rhs, nullptr);
                return Types::error();
            }
            Type* rt = check_expr(*e.rhs, lt->elem);
            if (rt != lt->elem && !rt->is_error())
                diag_.error(e.rhs->loc, "'??' default: expected " + type_name(lt->elem) + ", found " + type_name(rt));
            return lt->elem;
        }
        if (op == Tok::PlusOpt || op == Tok::MinusOpt || op == Tok::StarOpt || op == Tok::SlashOpt ||
            op == Tok::PercentOpt) {
            Type* t = check_operands(e, expected && expected->kind == TypeKind::Optional ? expected->elem : nullptr);
            if (t->is_error()) return t;
            if (!t->is_int()) {
                diag_.error(e.loc, "'" + op_text(op) + "' works on integers, found " + type_name(t));
                return Types::error();
            }
            return Types::optional(t);  // none bila overflow / bagi nol
        }

        Type* t = check_operands(e, expected);
        if (t->is_error()) return t;
        bool bitwise = op == Tok::Amp || op == Tok::Pipe || op == Tok::Caret;
        bool special = op == Tok::PlusWrap || op == Tok::MinusWrap || op == Tok::StarWrap || op == Tok::PlusSat ||
                       op == Tok::MinusSat || op == Tok::StarSat;
        if (bitwise || special) {
            if (!t->is_int()) {
                diag_.error(e.loc, "'" + op_text(op) + "' requires integer operands, found " + type_name(t));
                return Types::error();
            }
        } else if (!t->is_numeric()) {
            diag_.error(e.loc, "'" + op_text(op) + "' requires numeric operands, found " + type_name(t));
            return Types::error();
        }
        if (t->is_int()) set_range(e, arith(op, t, range(*e.lhs), range(*e.rhs), e.loc, expr_str(e)));
        return t;
    }

    Type* check_shift(Expr& e, Type* expected) {
        Type* lt = check_expr(*e.lhs, expected);
        Type* rt = check_expr(*e.rhs, nullptr);
        if (lt->is_error() || rt->is_error()) return Types::error();
        if (!lt->is_int() || !rt->is_int()) {
            diag_.error(e.loc, "'" + op_text(e.op) + "' requires integer operands");
            return Types::error();
        }
        Interval k = range(*e.rhs);
        Interval valid = Interval::of(Big::of_u64(0), Big::of_u64((uint64_t)lt->bits - 1));
        if (!(valid.lo <= k.lo && k.hi <= valid.hi)) {
            diag_.error(e.rhs->loc, "shift amount '" + expr_str(*e.rhs) + "' may be outside 0.." +
                                        std::to_string(lt->bits - 1) + " (range " + k.str() + ")",
                        "mask it (e.g. '" + expr_str(*e.rhs) + " & " + std::to_string(lt->bits - 1) +
                            "') or check it with 'if'");
            k = intersect(k, valid);
            if (k.empty()) k = valid;
        }
        Interval a = range(*e.lhs);
        Interval r = e.op == Tok::Shl ? ishl(a, k) : ishr(a, k);
        if (e.op == Tok::Shl && !fits(r, lt)) {
            if (lt->is_signed) {
                overflow_error(e.loc, expr_str(e), lt, r, Tok::Shl);
                r = clamp_to(r, lt);
            } else {
                r = type_range(lt);  // unsigned: bit yang keluar dibuang (terdefinisi)
            }
        }
        set_range(e, r);
        return lt;
    }

    bool printable(Type* t) {
        if (t->is_array_like() || t->kind == TypeKind::Optional) return printable(t->elem);
        if (t->kind == TypeKind::Enum || t->kind == TypeKind::Handle || t->kind == TypeKind::Link) return true;
        return t->is_numeric() || t == Types::bool_() || t == Types::str_lit() || t->kind == TypeKind::Struct;
    }

    // Rentang panjang array dari ekspresi bertipe array tumbuh.
    Interval dyn_len_of(const Expr& v) {
        Interval any = Interval::of(Big::of_u64(0), kMaxLen);
        switch (v.kind) {
            case ExprKind::ArrayLit: return Interval::exact(Big::of_u64(v.args.size()));
            case ExprKind::StrLit: return Interval::exact(Big::of_u64(v.str.size()));
            case ExprKind::Call:
                if (v.builtin == Builtin::Copy && !v.args.empty()) return array_len(*v.args[0]);
                return any;
            default: return any;
        }
    }

    Interval array_len(const Expr& a) {
        if (!a.type) return Interval::of(Big::of_u64(0), kMaxLen);
        if (a.type->kind == TypeKind::Array) return Interval::exact(Big::of_u64(a.type->count));
        if (a.kind == ExprKind::StrLit) return Interval::exact(Big::of_u64(a.str.size()));
        Path ap;
        if (path_of(a, ap)) return path_range(env_, len_of(ap), Types::i64());
        if (a.kind == ExprKind::SliceOf) return range(a);  // rentang panjang slice
        return Interval::of(Big::of_u64(0), kMaxLen);
    }

    // xs[a..b]: tampilan baca-saja; 0 <= a <= b <= len(xs) harus terbukti.
    Type* check_slice_of(Expr& e) {
        Type* at = check_expr(*e.lhs, nullptr);
        Type* sa = e.rhs ? check_expr(*e.rhs, nullptr) : Types::i64();
        Type* sb = e.end ? check_expr(*e.end, nullptr) : Types::i64();
        if (at->is_error() || sa->is_error() || sb->is_error()) return Types::error();
        if (!at->is_array_like()) {
            diag_.error(e.loc, "cannot slice a value of type " + type_name(at));
            return Types::error();
        }
        if (!is_place(*e.lhs) && e.lhs->kind != ExprKind::SliceOf) {
            diag_.error(e.lhs->loc, "slice a variable: store the array in a variable first");
            return Types::error();
        }
        if ((e.rhs && sa != Types::i64()) || (e.end && sb != Types::i64())) {
            diag_.error(e.loc, "slice bounds must be i64", "convert with 'as i64'");
            return Types::slice(at->elem);
        }
        Path ap;
        bool has_ap = at->kind != TypeKind::Array && path_of(*e.lhs, ap);
        Interval len = array_len(*e.lhs);
        Interval ra = e.rhs ? range(*e.rhs) : Interval::exact(Big::of_u64(0));
        Interval rb = e.end ? range(*e.end) : len;
        Path pa, pb;
        bool a_path = e.rhs && path_of(*e.rhs, pa), b_path = e.end && path_of(*e.end, pb);
        auto below_len = [&](const Interval& r, bool has_p, const Path& p, bool strict) {
            if (strict ? r.hi < len.lo : r.hi <= len.lo) return true;
            if (has_p && has_ap && env_.lt.count({p, ap})) return true;  // p < len
            if (!strict && has_p && has_ap && p == len_of(ap)) return true;  // p == len(xs)
            return false;
        };
        bool ok = !ra.lo.is_negative() && !rb.lo.is_negative();
        bool b_ok = !e.end || below_len(rb, b_path, pb, false);
        // xs[..len(xs) - k] dengan k >= 0
        if (!b_ok && e.end && e.end->kind == ExprKind::Binary && e.end->op == Tok::Minus && has_ap) {
            Path lp;
            if (path_of(*e.end->lhs, lp) && lp == len_of(ap) && !range(*e.end->rhs).lo.is_negative()) b_ok = true;
        }
        bool a_le_b = e.end ? (ra.hi <= rb.lo || (a_path && b_path && pa == pb))
                            : below_len(ra, a_path, pa, false);
        if (!e.rhs) a_le_b = true;
        if (!(ok && b_ok && a_le_b) && !env_.dead) {
            diag_.error(e.loc, "cannot prove slice bounds 0 <= start <= end <= len(" + expr_str(*e.lhs) + ")",
                        "check them first, e.g. 'if a <= b && b <= len(" + expr_str(*e.lhs) + ") { ... }'");
        }
        // rentang panjang slice: end - start
        Interval sl = isub(rb, ra);
        sl = intersect(sl, Interval::of(Big::of_u64(0), kMaxLen));
        set_range(e, sl.empty() ? Interval::of(Big::of_u64(0), kMaxLen) : sl);
        return Types::slice(at->elem);
    }

    // Bagian bersama a[i] (rvalue dan lvalue): tipe indeks + bukti batas.
    Type* check_index_part(Expr& e, Type* at) {
        Type* it = check_expr(*e.rhs, nullptr);
        if (!at->is_array_like()) {
            diag_.error(e.loc, "cannot index a value of type " + type_name(at));
            return Types::error();
        }
        if (it->is_error()) return at->elem;
        if (!it->is_int()) {
            diag_.error(e.rhs->loc, "array index must be an integer, found " + type_name(it));
            return at->elem;
        }
        Interval r = range(*e.rhs);
        std::string arr_text = expr_str(*e.lhs), idx_text = expr_str(*e.rhs);
        if (r.lo.is_negative()) {
            diag_.error(e.rhs->loc, "index '" + idx_text + "' may be negative (range " + r.str() + ")",
                        "use an unsigned index, or check it first: 'if " + idx_text + " < 0 { return ... }'");
            return at->elem;
        }
        bool ok = false;
        Interval len;
        if (at->kind == TypeKind::Array) {
            len = Interval::exact(Big::of_u64(at->count));
            ok = r.hi < len.lo;
        } else {
            Path ap;
            if (path_of(*e.lhs, ap)) {
                len = path_range(env_, len_of(ap), Types::i64());
                ok = r.hi < len.lo;
                Path ip;
                if (!ok && path_of(*e.rhs, ip) && env_.lt.count({ip, ap})) ok = true;
                // xs[len(xs) - k] dengan k >= 1, atau xs[j - k] dengan j < len(xs) dan k >= 0
                // (indeks tidak negatif sudah dicek di atas)
                if (!ok && e.rhs->kind == ExprKind::Binary && e.rhs->op == Tok::Minus) {
                    Path lp;
                    Interval k = range(*e.rhs->rhs);
                    if (path_of(*e.rhs->lhs, lp) && !k.lo.is_negative()) {
                        if (lp == len_of(ap) && !k.lo.is_zero()) ok = true;
                        if (env_.lt.count({lp, ap})) ok = true;
                    }
                }
            } else {
                len = e.lhs->kind == ExprKind::SliceOf ? range(*e.lhs) : Interval::of(Big::of_u64(0), kMaxLen);
                ok = r.hi < len.lo;
            }
        }
        if (!ok && !env_.dead) {
            std::string help = at->kind == TypeKind::Array
                                   ? "check it first, e.g. 'if " + idx_text + " >= " + std::to_string(at->count) +
                                         " { return ... }', or iterate with 'for i := range " + arr_text + "'"
                                   : "check it first, e.g. 'if " + idx_text + " < len(" + arr_text +
                                         ") { ... }', or iterate with 'for i, x := range " + arr_text + "'";
            diag_.error(e.loc, "cannot prove index '" + idx_text + "' is within the bounds of '" + arr_text +
                                   "' (index range " + r.str() + ", length " + len.str() + ")",
                        help);
        }
        if (at->elem->is_int()) set_range(e, type_range(at->elem));
        return at->elem;
    }

    Type* check_array_lit(Expr& e) {
        Type* t = resolve_type(e.array_type);
        if (t->is_error()) {
            for (auto& a : e.args) check_expr(*a, nullptr);
            return t;
        }
        if (t->kind == TypeKind::Slice) {
            diag_.error(e.loc, "a slice literal has no array to borrow from", "use '[N]T{...}' or '[..]T{...}'");
            return Types::error();
        }
        if (!t->is_array_like()) {
            diag_.error(e.loc, "expected an array type in the literal, found " + type_name(t));
            return Types::error();
        }
        if (t->kind == TypeKind::Array && !e.args.empty() && e.args.size() != t->count)
            diag_.error(e.loc, "array literal has " + std::to_string(e.args.size()) + " element(s), but " +
                                   type_name(t) + " needs " + std::to_string(t->count),
                        "give every element, or write '" + type_name(t) + "{}' for all zeros");
        for (auto& a : e.args) {
            Type* at = check_expr(*a, t->elem);
            if (at != t->elem && !at->is_error())
                diag_.error(a->loc, "array element: expected " + type_name(t->elem) + ", found " + type_name(at));
        }
        return t;
    }

    // Argumen untuk parameter slice: array apa pun dengan elemen sama (dan string untuk []u8).
    static bool coerces(const Type* at, const Type* pt) {
        if (at == pt) return true;
        return pt->kind == TypeKind::Slice && at->is_array_like() && at->elem == pt->elem;
    }

    Type* check_array_builtin(Expr& e) {
        const std::string& n = e.str;
        if (n == "len" || n == "copy") {
            e.builtin = n == "len" ? Builtin::Len : Builtin::Copy;
            if (e.args.size() != 1) {
                diag_.error(e.loc, "'" + n + "' takes exactly one argument");
                for (auto& a : e.args) check_expr(*a, nullptr);
                return Types::error();
            }
            Type* at = check_expr(*e.args[0], Types::slice(Types::int_(8, false)));
            if (at->kind == TypeKind::Slice && e.args[0]->kind == ExprKind::StrLit) {
                // len("abc") / copy("abc")
            } else if (at->is_error()) {
                return Types::error();
            } else if (!at->is_array_like()) {
                diag_.error(e.args[0]->loc, "'" + n + "' needs an array, found " + type_name(at));
                return Types::error();
            } else if (e.args[0]->kind != ExprKind::Name && at->kind != TypeKind::Array) {
                // (array tumbuh sementara sudah dilaporkan oleh check_expr)
            }
            if (e.builtin == Builtin::Len) {
                Path lp;
                if (path_of(e, lp)) set_range(e, path_range(env_, lp, Types::i64()));
                else set_range(e, array_len(*e.args[0]));
                return Types::i64();
            }
            return Types::dyn(at->elem);
        }
        return nullptr;
    }

    // Argumen metode bawaan dengan jumlah tertentu; mengembalikan false bila salah.
    bool builtin_args(Expr& e, size_t n) {
        if (e.args.size() == n + 1) return true;
        diag_.error(e.loc, "'" + e.str + "' takes " + std::to_string(n) + " argument(s)");
        for (size_t i = 1; i < e.args.size(); i++) check_expr(*e.args[i], nullptr);
        return false;
    }

    void expect_arg(Expr& a, Type* want, const std::string& what) {
        Type* t = check_expr(a, want);
        if (t != want && !t->is_error()) diag_.error(a.loc, what + ": expected " + type_name(want) + ", found " + type_name(t));
    }

    // Pool[T]: handle dengan generasi. get/set/remove memeriksa generasi saat runtime:
    // handle basi menghasilkan none/false (bukan data milik pemilik baru, bukan crash).
    Type* check_pool_method(Expr& e, Type* pt) {
        Type* el = pt->elem;
        Type* h = Types::wrap(TypeKind::Handle, el);
        Expr& recv = *e.args[0];
        const std::string& n = e.str;
        bool mutates = n == "add" || n == "set" || n == "remove";
        if (mutates && !is_place(recv)) diag_.error(recv.loc, "'" + n + "' needs a pool variable");
        if (mutates) invalidate_root(place_root(recv));
        if (n == "add") {
            e.builtin = Builtin::PoolAdd;
            if (builtin_args(e, 1)) expect_arg(*e.args[1], el, "add");
            return h;
        }
        if (n == "get" || n == "contains" || n == "remove") {
            e.builtin = n == "get" ? Builtin::PoolGet : n == "contains" ? Builtin::PoolContains : Builtin::PoolRemove;
            if (builtin_args(e, 1)) expect_arg(*e.args[1], h, n);
            return n == "get" ? Types::optional(el) : Types::bool_();
        }
        if (n == "set") {
            e.builtin = Builtin::PoolSet;
            if (builtin_args(e, 2)) {
                expect_arg(*e.args[1], h, "set");
                expect_arg(*e.args[2], el, "set");
            }
            return Types::bool_();
        }
        if (n == "count") {
            e.builtin = Builtin::PoolCount;
            builtin_args(e, 0);
            return Types::i64();
        }
        diag_.error(e.loc, "Pool has no method '" + n + "'", "methods: add, get, set, remove, contains, count");
        return Types::error();
    }

    // Link[T]: offset u32 bertipe dari awal array. Selalu dicek saat dipakai (none/false bila di luar batas).
    Type* check_link_method(Expr& e, Type* at) {
        Type* el = at->elem;
        Type* lt = Types::wrap(TypeKind::Link, el);
        Expr& recv = *e.args[0];
        const std::string& n = e.str;
        if (n == "link") {
            e.builtin = Builtin::Link;
            if (builtin_args(e, 1)) {
                Type* it = check_expr(*e.args[1], nullptr);
                if (!it->is_int() && !it->is_error()) diag_.error(e.args[1]->loc, "link: index must be an integer");
            }
            return Types::optional(lt);
        }
        if (n == "at") {
            e.builtin = Builtin::At;
            if (builtin_args(e, 1)) expect_arg(*e.args[1], lt, "at");
            return Types::optional(el);
        }
        // push_link / put mengubah array
        if (at->kind == TypeKind::Slice || !is_place(recv)) {
            diag_.error(e.loc, "'" + n + "' needs a growable array or fixed array variable (slices are read-only)");
            return Types::error();
        }
        if (n == "push_link") {
            if (at->kind != TypeKind::Dyn) {
                diag_.error(e.loc, "only growable arrays '[..]T' can push");
                return Types::error();
            }
            e.builtin = Builtin::PushLink;
            if (builtin_args(e, 1)) expect_arg(*e.args[1], el, "push_link");
            Path rp;
            if (path_of(recv, rp)) {
                Path lp = len_of(rp);
                Interval len = path_range(env_, lp, Types::i64());
                Interval nl = intersect(iadd(len, Interval::exact(Big::of_u64(1))), Interval::of(Big::of_u64(0), kMaxLen));
                set_path_range(env_, lp, Types::i64(), nl.empty() ? Interval::of(Big::of_u64(0), kMaxLen) : nl);
            } else {
                invalidate_root(place_root(recv));
            }
            return Types::optional(lt);
        }
        e.builtin = Builtin::Put;
        if (builtin_args(e, 2)) {
            expect_arg(*e.args[1], lt, "put");
            expect_arg(*e.args[2], el, "put");
        }
        return Types::bool_();
    }

    // Tipe "datar": bisa disalin byte demi byte ke/dari to_bytes (tanpa pointer, region, atau fungsi).
    // Link di dalamnya harus menunjuk ke tipe elemen yang sama (divalidasi terhadap jumlah elemen).
    bool flat_type(const Type* t, const Type* top, std::string& why, std::set<const Type*>& seen) {
        switch (t->kind) {
            case TypeKind::Int:
            case TypeKind::Float:
            case TypeKind::Bool:
            case TypeKind::Handle: return true;
            case TypeKind::Link:
                if (t->elem != top) {
                    why = "a " + type_name(t) + " inside " + type_name(top) +
                          " cannot be checked when loading (only Link[" + type_name(top) + "] can)";
                    return false;
                }
                return true;
            case TypeKind::Array:
            case TypeKind::Optional: return flat_type(t->elem, top, why, seen);
            case TypeKind::Struct:
                if (!seen.insert(t).second) return true;
                if (t->sd->fields.empty()) {
                    why = "empty struct " + type_name(t) + " has no bytes";
                    return false;
                }
                for (auto& f : t->sd->fields)
                    if (!f.type || !flat_type(f.type, top, why, seen)) return false;
                return true;
            case TypeKind::Enum:
                for (auto& v : t->ed->variants)
                    if (v.payload && !flat_type(v.payload->type, top, why, seen)) return false;
                return true;
            default:
                why = type_name(t) + " holds pointers or memory regions";
                return false;
        }
    }

    // run worker(args): goroutine baru. Argumen disalin (data datar & channel); tidak ada closure,
    // jadi goroutine tidak bisa menyentuh variabel milik goroutine lain.
    Type* check_spawn(Expr& e) {
        FnDecl* fn = named(fns_, e.str, e.loc);
        if (!fn || fn->is_template || fn->is_extern || fn->has_receiver) {
            diag_.error(e.loc, !fn ? "unknown function '" + e.str + "'"
                               : fn->is_template ? "'run' needs a non-generic function without function parameters"
                                                 : "'run' needs a plain tree-lang function",
                        "declare 'fn " + e.str + "(...)' and start it with 'run " + e.str + "(...)'");
            for (auto& a : e.args) check_expr(*a, nullptr);
            return Types::thread_();
        }
        e.fn = fn;
        run_targets_.insert(fn);
        if (e.args.size() != fn->param_types.size())
            diag_.error(e.loc, "'" + fn->name + "' expects " + std::to_string(fn->param_types.size()) +
                                   " argument(s), found " + std::to_string(e.args.size()));
        for (size_t i = 0; i < e.args.size(); i++) {
            Type* pt = i < fn->param_types.size() ? fn->param_types[i] : nullptr;
            if (pt && !pt->is_error() && !sendable(pt)) {
                diag_.error(e.args[i]->loc, "argument " + std::to_string(i + 1) + " of '" + fn->name +
                                                "' would be shared with the new goroutine (" + type_name(pt) + ")",
                            "goroutines share no memory: pass plain data or a channel, and send owned data through it");
                check_expr(*e.args[i], nullptr);
                continue;
            }
            Type* at = check_expr(*e.args[i], pt);
            if (pt && at != pt && !at->is_error() && !pt->is_error())
                diag_.error(e.args[i]->loc, "argument " + std::to_string(i + 1) + " of '" + fn->name + "': expected " +
                                                type_name(pt) + ", found " + type_name(at));
        }
        return Types::thread_();
    }

    // close(ch) / wait(task) -> bool
    Type* check_thread_builtin(Expr& e) {
        const std::string& n = e.str;
        if (e.args.size() != 1) {
            diag_.error(e.loc, "'" + n + "' takes one argument");
            for (auto& a : e.args) check_expr(*a, nullptr);
            return Types::error();
        }
        Expr& a = *e.args[0];
        Type* t = check_expr(a, nullptr);
        if (n == "wait") {
            e.builtin = Builtin::Wait;
            if (t != Types::thread_() && !t->is_error()) diag_.error(a.loc, "wait: expected a Task, found " + type_name(t));
            return Types::bool_();
        }
        e.builtin = Builtin::Close;
        if (t->kind != TypeKind::Chan && !t->is_error()) diag_.error(a.loc, "close: expected a channel, found " + type_name(t));
        return Types::void_();
    }

    Type* check_chan_make(Expr& e) {
        Type* el = resolve_type(e.array_type);
        if (e.args.size() != 1) {
            diag_.error(e.loc, "'chan[T](n)' takes the capacity");
            for (auto& a : e.args) check_expr(*a, nullptr);
        } else {
            Type* ct = check_expr(*e.args[0], Types::i64());
            if (!ct->is_error() && !ct->is_int()) diag_.error(e.args[0]->loc, "channel capacity must be an integer");
            Interval r = range(*e.args[0]);
            if (ct->is_int() && r.lo.is_negative())
                diag_.error(e.args[0]->loc, "channel capacity may be negative (range " + r.str() + ")",
                            "check it first, or use 0 for an unbuffered channel");
        }
        if (el->is_error()) return el;
        if (!chan_elem_ok(el)) {
            diag_.error(e.loc, "channels carry plain data, channels, or owned arrays (moved), not " + type_name(el));
            return Types::error();
        }
        return Types::wrap(TypeKind::Chan, el);
    }

    // Kirim nilai v ke channel bertipe elem. Variabel pemilik dipindahkan (tidak bisa dipakai lagi).
    void check_chan_value(Expr& v, Type* el) {
        if (contains_dyn(el)) {
            if (v.kind == ExprKind::Name) {
                Type* t = check_expr(v, el);
                if (t != el && !t->is_error()) diag_.error(v.loc, "expected " + type_name(el) + ", found " + type_name(t));
                VarInfo* var = v.var;
                if (var && (var->is_param || var->is_ref))
                    diag_.error(v.loc, "only a local variable can be moved into a channel",
                                "send 'copy(" + v.str + ")' or move a local array");
                else if (var && is_borrowed(var))
                    diag_.error(v.loc, "cannot move '" + v.str + "' while it is borrowed");
                else if (var) {
                    env_.moved.insert(var);
                    v.move_src = true;
                }
                return;
            }
            Type* t = check_owned(v, el);
            if (t != el && !t->is_error()) diag_.error(v.loc, "expected " + type_name(el) + ", found " + type_name(t));
            return;
        }
        Type* t = check_expr(v, el);
        if (t != el && !t->is_error()) diag_.error(v.loc, "expected " + type_name(el) + ", found " + type_name(t));
    }

    Type* chan_of(Expr& ch) {
        Type* t = check_expr(ch, nullptr);
        if (t->is_error()) return nullptr;
        if (t->kind != TypeKind::Chan) {
            diag_.error(ch.loc, "expected a channel, found " + type_name(t));
            return nullptr;
        }
        return t;
    }

    Type* check_chan_send(Expr& e) {
        Type* ct = chan_of(*e.lhs);
        if (!ct) {
            check_expr(*e.rhs, nullptr);
            return Types::bool_();
        }
        check_chan_value(*e.rhs, ct->elem);
        return Types::bool_();
    }

    // <-ch : T? (none bila channel tertutup dan kosong)
    Type* check_chan_recv(Expr& e) {
        Type* ct = chan_of(*e.lhs);
        if (!ct) return Types::error();
        if (contains_dyn(ct->elem) && owned_recv_ok_ != &e) {
            diag_.error(e.loc, "receive owned values straight into a variable",
                        "write 'if some x := <-ch { ... }' or 'for x := range ch { ... }'");
        }
        return Types::optional(ct->elem);
    }

    // for x := range ch : berhenti saat channel tertutup dan kosong
    void check_range_chan(Stmt& s, Type* ct) {
        s.range_chan = true;
        if (s.e1) diag_.error(s.e1->loc, "'a..b' ranges are for integers");
        if (s.names.size() > 1) diag_.error(s.name_locs[1], "'for x := range ch' takes one variable");
        Type* el = ct->elem;
        Env entry = env_;
        auto run = [&]() {
            s.vars.clear();
            scopes_.emplace_back();
            VarInfo* xv = new_var(s.names[0], s.name_locs[0], el, true);
            s.vars.push_back(xv);
            if (xv->name != "_") declare(xv);
            env_.init.insert(xv);
            env_.moved.erase(xv);
            loops_.emplace_back();
            check_block(*s.body);
            LoopFrame f = std::move(loops_.back());
            loops_.pop_back();
            scopes_.pop_back();
            Env be = env_;
            for (Env& c : f.conts) be = join(be, c);
            last_breaks_ = std::move(f.breaks);
            return be;
        };
        Env h = solve_loop(entry, *s.body, {s.e2.get()}, run, Big::pos_inf(), false);
        env_ = h;
        run();
        Env exit = h;
        for (Env& b : last_breaks_) exit = join(exit, b);
        env_ = exit;
    }

    // select: menunggu salah satu cabang siap (acak-adil bila beberapa siap)
    Expr select_dummy_{ExprKind::IntLit, Loc{}};
    void check_select(Stmt& s) {
        Env base = env_;
        Env out = base;
        out.dead = true;
        int timeouts = 0, defaults = 0;
        if (s.sel.empty()) diag_.error(s.loc, "select needs at least one arm");
        for (auto& arm : s.sel) {
            env_ = base;
            arm.var = nullptr;
            Type* bind_t = nullptr;
            switch (arm.kind) {
                case SelectArm::Recv: {
                    Type* ct = chan_of(*arm.op->lhs);
                    arm.op->type = ct ? Types::optional(ct->elem) : Types::error();
                    if (ct && contains_dyn(ct->elem))
                        diag_.error(arm.loc, "select cannot receive owned values yet",
                                    "use 'if some x := <-ch' or 'for x := range ch'");
                    if (ct) bind_t = Types::optional(ct->elem);
                    break;
                }
                case SelectArm::Send: {
                    Type* ct = chan_of(*arm.op->lhs);
                    arm.op->type = Types::bool_();
                    if (ct && contains_dyn(ct->elem))
                        diag_.error(arm.loc, "select cannot send owned values yet (the value would be lost if "
                                             "another arm is chosen)");
                    else if (ct) check_chan_value(*arm.op->rhs, ct->elem);
                    bind_t = Types::bool_();
                    break;
                }
                case SelectArm::Timeout: {
                    timeouts++;
                    if (arm.op) {
                        Type* t = check_expr(*arm.op, Types::i64());
                        if (t != Types::i64() && !t->is_error()) diag_.error(arm.op->loc, "timeout takes milliseconds (i64)");
                    }
                    break;
                }
                case SelectArm::Default: defaults++; break;
            }
            if (!arm.bind.empty() && !bind_t) diag_.error(arm.bind_loc, "this select arm has no value to bind");
            check_with_binding(*arm.body, bind_t ? arm.bind : "", arm.bind_loc, bind_t ? bind_t : Types::error(), false,
                               select_dummy_, &arm.var);
            out = join(out, env_);
        }
        if (timeouts > 1 || defaults > 1 || (timeouts && defaults))
            diag_.error(s.loc, "a select has at most one 'timeout' or one 'default' arm");
        env_ = out;
    }

    // to_bytes(x) -> [..]u8   /   from_bytes(bytes, &out) -> bool
    Type* check_bytes_builtin(Expr& e) {
        bool to = e.str == "to_bytes";
        e.builtin = to ? Builtin::ToBytes : Builtin::FromBytes;
        if (e.args.size() != (to ? 1u : 2u)) {
            diag_.error(e.loc, to ? "'to_bytes' takes one value" : "'from_bytes' takes (bytes, &out)");
            for (auto& a : e.args) check_expr(*a, nullptr);
            return Types::error();
        }
        Type* vt;
        if (to) {
            vt = check_expr(*e.args[0], nullptr);
        } else {
            Type* bt = check_expr(*e.args[0], Types::slice(Types::int_(8, false)));
            if (!bt->is_error() && !(bt->is_array_like() && bt->elem == Types::int_(8, false)))
                diag_.error(e.args[0]->loc, "from_bytes: expected bytes ([..]u8 / []u8), found " + type_name(bt));
            Expr& out = *e.args[1];
            if (!(out.kind == ExprKind::Unary && out.op == Tok::Amp)) {
                check_expr(out, nullptr);
                diag_.error(out.loc, "from_bytes writes into its second argument: pass '&" + expr_str(out) + "'");
                return Types::bool_();
            }
            vt = check_lvalue(*out.lhs, true);
            out.type = vt->is_error() ? vt : Types::ref(vt);
            VarInfo* root = place_root(*out.lhs);
            if (root && is_borrowed(root))
                diag_.error(out.loc, "cannot replace '" + expr_str(*out.lhs) + "' while it is borrowed");
            invalidate_root(root);
            if (vt->kind == TypeKind::Array || vt->kind == TypeKind::Slice) {
                diag_.error(out.loc, "from_bytes needs a growable array '[..]T' or a single value to load into");
                return Types::bool_();
            }
        }
        if (vt->is_error()) return to ? Types::dyn(Types::int_(8, false)) : Types::bool_();
        Type* el = vt->is_array_like() ? vt->elem : vt;
        std::string why;
        std::set<const Type*> seen;
        if (!flat_type(el, el, why, seen))
            diag_.error(e.loc, "cannot serialize " + type_name(vt) + ": " + why,
                        "only plain data (numbers, bool, structs, fixed arrays, enums, optionals, Link[T]) can be stored");
        return to ? Types::dyn(Types::int_(8, false)) : Types::bool_();
    }

    // xs.get(i) -> T?   (tanpa bukti indeks: di luar batas menghasilkan none)
    Type* check_get(Expr& e, Type* rt) {
        e.builtin = Builtin::Get;
        if (e.args.size() != 2) {
            diag_.error(e.loc, "'get' takes exactly one index");
            return Types::error();
        }
        Type* it = check_expr(*e.args[1], nullptr);
        if (!it->is_int() && !it->is_error()) diag_.error(e.args[1]->loc, "index must be an integer");
        return Types::optional(rt->elem);
    }

    Type* check_dyn_method(Expr& e, Type* rt) {
        e.builtin = e.str == "push" ? Builtin::Push : Builtin::Append;
        Expr& recv = *e.args[0];
        if (rt->kind != TypeKind::Dyn) {
            diag_.error(e.loc, "only growable arrays '[..]T' can " + e.str + ", found " + type_name(rt));
            for (size_t i = 1; i < e.args.size(); i++) check_expr(*e.args[i], nullptr);
            return Types::void_();
        }
        if (!is_place(recv)) {
            diag_.error(recv.loc, "'" + e.str + "' needs a variable, field or element");
            return Types::void_();
        }
        if (e.args.size() != 2) {
            diag_.error(e.loc, "'" + e.str + "' takes exactly one argument");
            for (size_t i = 1; i < e.args.size(); i++) check_expr(*e.args[i], nullptr);
            return Types::void_();
        }
        Path rp;
        bool tracked = path_of(recv, rp);
        Path lp = tracked ? len_of(rp) : Path{};
        Interval len = tracked ? path_range(env_, lp, Types::i64()) : Interval::of(Big::of_u64(0), kMaxLen);
        Interval add;
        if (e.builtin == Builtin::Push) {
            Type* vt = check_expr(*e.args[1], rt->elem);
            if (vt != rt->elem && !vt->is_error())
                diag_.error(e.args[1]->loc, "push: expected " + type_name(rt->elem) + ", found " + type_name(vt));
            add = Interval::exact(Big::of_u64(1));
        } else {
            Type* vt = check_expr(*e.args[1], Types::slice(rt->elem));
            if (!vt->is_error() && !coerces(vt, Types::slice(rt->elem)))
                diag_.error(e.args[1]->loc, "append: expected an array of " + type_name(rt->elem) + ", found " +
                                                type_name(vt));
            add = array_len(*e.args[1]);
        }
        grow_by_[&e] = add;
        // panjang hanya bertambah: fakta 'i < len(xs)' tetap berlaku
        Interval nl = iadd(len, add);
        nl = intersect(nl, Interval::of(Big::of_u64(0), kMaxLen));
        if (nl.empty()) nl = Interval::of(Big::of_u64(0), kMaxLen);
        if (tracked) set_path_range(env_, lp, Types::i64(), nl);
        else invalidate_root(place_root(recv));  // lewat referensi lokal
        return Types::void_();
    }

    Type* check_call(Expr& e) {
        if (!e.method_call && e.str == "str" && !named(fns_, "str", e.loc)) {
            // str(a, b, ...) -> [..]u8 : format yang sama dengan print, ke dalam string
            e.builtin = Builtin::Str;
            for (auto& a : e.args) {
                Type* t = check_expr(*a, nullptr);
                if (!t->is_error() && !printable(t)) diag_.error(a->loc, "cannot format a value of type " + type_name(t));
            }
            return Types::dyn(Types::int_(8, false));
        }
        if (!e.method_call && e.str == "panic" && !named(fns_, "panic", e.loc)) return check_panic(e);
        if (!e.method_call && e.str == "recover" && !named(fns_, "recover", e.loc)) return check_recover(e);
        if (!e.method_call && (e.str == "print" || e.str == "abort")) {
            e.builtin = e.str == "print" ? Builtin::Print : Builtin::Abort;
            if (e.builtin == Builtin::Abort && !trusted(e.loc)) panics_[&e] = {e.loc, loc_text(e.loc) + ": abort(...)"};
            for (auto& a : e.args) {
                Type* t = check_expr(*a, nullptr);
                if (t->is_error()) continue;
                if (e.builtin == Builtin::Abort ? t != Types::str_lit() : !printable(t))
                    diag_.error(a->loc, e.builtin == Builtin::Abort
                                            ? "'abort' takes a string literal message"
                                            : "cannot print a value of type " + type_name(t));
            }
            if (e.builtin == Builtin::Abort && e.args.size() > 1)
                diag_.error(e.loc, "'abort' takes at most one argument");
            return Types::void_();
        }
        if (!e.method_call) {
            // f(x) lewat parameter fungsi
            VarInfo* fv = lookup(e.str, e.loc);
            if (fv && fv->type->kind == TypeKind::Fn) return check_fn_param_call(e, fv);
            if (fv && fv->type->kind == TypeKind::FuncRef) {
                // h(c): panggilan lewat referensi fungsi
                if (env_.moved.count(fv)) diag_.error(e.loc, "'" + fv->name + "' was moved into a channel");
                if (!fv->is_const && !env_.dead && !env_.init.count(fv))
                    diag_.error(e.loc, "'" + fv->name + "' may be used before it is set");
                e.var = fv;
                return check_call_args(e, fv->type->elems, fv->type->elem, fv->name);
            }
        }
        if (!e.method_call && !named(fns_, e.str, e.loc) && (e.str == "to_bytes" || e.str == "from_bytes"))
            return check_bytes_builtin(e);
        if (!e.method_call && !named(fns_, e.str, e.loc) && (e.str == "close" || e.str == "wait"))
            return check_thread_builtin(e);
        if (!e.method_call && !named(fns_, e.str, e.loc))
            if (Type* t = check_array_builtin(e)) return t;
        FnDecl* fn = nullptr;
        if (e.method_call) {
            Type* rt = check_expr(*e.args[0], nullptr);
            if (rt->is_error()) {
                for (size_t i = 1; i < e.args.size(); i++) check_expr(*e.args[i], nullptr);
                return Types::error();
            }
            if (rt->is_array_like() && (e.str == "push" || e.str == "append")) return check_dyn_method(e, rt);
            if (rt->is_array_like() && e.str == "get") return check_get(e, rt);
            if (rt->kind == TypeKind::Pool) return check_pool_method(e, rt);
            if (rt->is_array_like() && (e.str == "link" || e.str == "push_link" || e.str == "at" || e.str == "put"))
                return check_link_method(e, rt);
            if (type_key(rt)) {
                auto ms = methods_.find(type_key(rt));
                if (ms != methods_.end()) {
                    auto m = ms->second.find(e.str);
                    if (m != ms->second.end()) {
                        fn = m->second;
                        check_visible(fn->loc, e.loc, "method", e.str, type_name(rt));
                    }
                }
            }
            if (!fn && rt->is_array_like())
                if (FnDecl* f = named(fns_, e.str, e.loc)) {
                    // xs.filter() { ... }  =  filter(xs) { ... }
                    fn = f;
                    e.method_call = false;
                }
            if (!fn) {
                diag_.error(e.loc, "type " + type_name(rt) + " has no method '" + e.str + "'",
                            rt->kind == TypeKind::Struct ? "declare it with 'fn (x " + type_name(rt) + ") " + e.str +
                                                               "(...) { ... }'"
                                                         : "");
                for (size_t i = 1; i < e.args.size(); i++) check_expr(*e.args[i], nullptr);
                return Types::error();
            }
        } else {
            fn = named(fns_, e.str, e.loc);
            if (!fn) {
                diag_.error(e.loc, "unknown function '" + e.str + "'");
                for (auto& a : e.args) check_expr(*a, nullptr);
                return Types::error();
            }
        }
        if (fn->is_extern && !trusted(e.loc))
            unsafe_calls_[&e] = {e.loc, loc_text(e.loc) + ": call to unsafe extern fn '" + fn->name + "'"};
        if (fn->is_thread) {
            diag_.error(e.loc, "thread function '" + fn->name + "' is started with 'spawn " + fn->name + "(...)'");
            for (auto& a : e.args) check_expr(*a, nullptr);
            return Types::error();
        }
        if (fn->is_template) {
            fn = instantiate_call(e, fn);
            if (!fn) return Types::error();
        }
        e.fn = fn;
        if (!fn->ret) return Types::error();
        return check_call_args(e, fn->param_types, fn->ret, fn->name);
    }

    // Argumen panggilan (langsung atau lewat func): tipe, referensi '&', dan aturan anti-alias.
    Type* check_call_args(Expr& e, const std::vector<Type*>& pts, Type* ret, const std::string& fname) {
        size_t skip = e.method_call ? 1 : 0;  // receiver sudah diperiksa
        if (e.args.size() != pts.size()) {
            diag_.error(e.loc, "'" + fname + "' expects " + std::to_string(pts.size() - skip) +
                                   " argument(s), found " + std::to_string(e.args.size() - skip));
        }
        struct Use {
            VarInfo* root;
            Loc loc;
            std::string text;
        };
        std::vector<Use> muts, views;  // pinjaman '&' dan tampilan slice dalam panggilan ini
        if (e.method_call && !pts.empty() && pts[0]->kind == TypeKind::Ref) {
            Expr& recv = *e.args[0];
            if (!is_place(recv)) {
                diag_.error(recv.loc, "method '" + fname + "' takes '&" + type_name(pts[0]->elem) +
                                          "' and needs a variable, not a temporary value");
            } else {
                recv.borrow = true;
                muts.push_back({place_root(recv), recv.loc, expr_str(recv)});
            }
        }
        for (size_t i = skip; i < e.args.size(); i++) {
            Type* pt = i < pts.size() ? pts[i] : nullptr;
            Expr& a = *e.args[i];
            std::string which = "argument " + std::to_string(i + 1 - skip) + " of '" + fname + "'";
            if (pt && pt->kind == TypeKind::Fn) continue;  // sudah diperiksa saat instansiasi
            if (pt && pt->kind == TypeKind::Ref) {
                Type* at = nullptr;
                if (a.kind == ExprKind::Unary && a.op == Tok::Amp) {
                    at = check_lvalue(*a.lhs, true);
                    a.type = at->is_error() ? at : Types::ref(at);
                } else if (a.kind == ExprKind::Name && lookup(a.str, a.loc) && lookup(a.str, a.loc)->is_ref) {
                    at = check_expr(a, nullptr);  // meneruskan referensi yang sudah ada
                } else {
                    check_expr(a, nullptr);
                    diag_.error(a.loc, which + " is a reference: write '&" + expr_str(a) + "'",
                                "the function may modify it");
                    continue;
                }
                if (!at->is_error() && at != pt->elem)
                    diag_.error(a.loc, which + ": expected &" + type_name(pt->elem) + ", found &" + type_name(at));
                a.borrow = true;
                muts.push_back({place_root(a), a.loc, expr_str(a)});
                continue;
            }
            Type* at = check_expr(a, pt);
            if (pt && !coerces(at, pt) && !at->is_error() && !pt->is_error())
                diag_.error(a.loc, which + ": expected " + type_name(pt) + ", found " + type_name(at));
            if (pt && pt->kind == TypeKind::Slice && a.kind != ExprKind::StrLit && (is_place(a) || a.kind == ExprKind::SliceOf))
                views.push_back({place_root(a), a.loc, expr_str(a)});
        }
        // Aturan anti-alias: setiap root hanya boleh dipinjam '&' sekali per panggilan, tidak bersamaan
        // dengan tampilan slice-nya, dan tidak saat sedang dipinjam referensi lokal / loop range.
        for (size_t i = 0; i < muts.size(); i++) {
            VarInfo* r = muts[i].root;
            if (!r) continue;
            for (size_t j = 0; j < i; j++)
                if (muts[j].root == r)
                    diag_.error(muts[i].loc, "'" + r->name + "' is borrowed twice in the same call",
                                "a function may not receive two references to the same variable");
            for (auto& v : views)
                if (v.root == r)
                    diag_.error(muts[i].loc, "'" + r->name + "' is borrowed while also passed as a slice '" + v.text + "'",
                                "the function could replace the array the slice points into");
            if (is_borrowed(r))
                diag_.error(muts[i].loc, "cannot pass '" + muts[i].text + "' while '" + r->name + "' is borrowed",
                            "a reference ('r := &...') or a 'for ... range' loop is still reading it");
        }
        // block tidak boleh mengubah variabel yang juga diteruskan ke pemanggilan ini
        for (auto& a : e.args) {
            if (a->kind != ExprKind::BlockLit) continue;
            std::set<VarInfo*> mods;
            collect_assigned(*a->block, mods);
            for (auto* list : {&muts, &views})
                for (auto& u : *list)
                    if (u.root && mods.count(u.root))
                        diag_.error(a->loc, "the block modifies '" + u.root->name + "', which is also passed to '" +
                                                fname + "'",
                                    "the function could be using it while the block changes it");
        }
        for (auto& m : muts) invalidate_root(m.root);  // fungsi mungkin telah mengubahnya
        return ret;
    }

    // f(x) di mana f adalah parameter bertipe fungsi.
    Type* check_fn_param_call(Expr& e, VarInfo* fv) {
        Type* ft = fv->type;
        e.var = fv;
        if (!block_ctx_.empty() && block_ctx_.back().captures.count(fv)) {
            diag_.error(e.loc, "a block cannot call function parameters of the enclosing function yet");
            return ft->elem;
        }
        if (e.args.size() != ft->elems.size())
            diag_.error(e.loc, "'" + fv->name + "' expects " + std::to_string(ft->elems.size()) + " argument(s), found " +
                                   std::to_string(e.args.size()));
        for (size_t i = 0; i < e.args.size(); i++) {
            Type* pt = i < ft->elems.size() ? ft->elems[i] : nullptr;
            Type* at = check_expr(*e.args[i], pt);
            if (pt && at != pt && !at->is_error() && !pt->is_error())
                diag_.error(e.args[i]->loc, "argument " + std::to_string(i + 1) + " of '" + fv->name + "': expected " +
                                                type_name(pt) + ", found " + type_name(at));
        }
        return ft->elem;
    }

    // Panggilan ke template: inferensi parameter tipe, periksa block, lalu instansiasi.
    FnDecl* instantiate_call(Expr& e, FnDecl* tmpl) {
        const auto& tps = tmpl->type_params;
        if (e.args.size() != tmpl->params.size()) {
            diag_.error(e.loc, "'" + tmpl->name + "' expects " + std::to_string(tmpl->params.size()) +
                                   " argument(s), found " + std::to_string(e.args.size()));
            for (auto& a : e.args)
                if (a->kind != ExprKind::BlockLit) check_expr(*a, nullptr);
            return nullptr;
        }
        std::map<std::string, Type*> b;
        // 1) argumen biasa (literal tanpa tipe belakangan)
        diag_.push_mute();
        for (int pass = 0; pass < 2; pass++)
            for (size_t i = 0; i < e.args.size(); i++) {
                Expr& a = *e.args[i];
                const TypeExpr& te = tmpl->params[i].type;
                if (te_is_fn(te) || a.kind == ExprKind::BlockLit) continue;
                if ((pass == 0) == is_untyped_lit(a)) continue;
                Type* at;
                if (a.kind == ExprKind::Unary && a.op == Tok::Amp) at = check_lvalue(*a.lhs, true);
                else if (pass == 1 && all_bound(te, tps, b)) at = check_expr(a, resolve_with(te, b));
                else at = check_expr(a, nullptr);
                unify(te, at, tps, b);
            }
        diag_.pop_mute();
        // 2) argumen fungsi: trailing block, fungsi bernama, atau parameter fungsi yang diteruskan
        std::vector<FnBinding> binds(tmpl->params.size());
        std::vector<VarInfo*> amp_roots;
        for (auto& a : e.args)
            if (a->kind == ExprKind::Unary && a->op == Tok::Amp)
                if (VarInfo* r = syntactic_root(*a->lhs)) amp_roots.push_back(r);
        for (size_t i = 0; i < e.args.size(); i++) {
            const TypeExpr& te = tmpl->params[i].type;
            Expr& a = *e.args[i];
            if (!te_is_fn(te)) {
                if (a.kind == ExprKind::BlockLit)
                    diag_.error(a.loc, "argument " + std::to_string(i + 1) + " of '" + tmpl->name +
                                           "' is not a function parameter; a block cannot be passed here");
                continue;
            }
            std::vector<Type*> ptypes;
            bool bound = true;
            for (auto& pt : te.fn_params) {
                if (te_mentions(pt, tps) && !all_bound(pt, tps, b)) bound = false;
                ptypes.push_back(bound ? resolve_with(pt, b) : Types::error());
            }
            if (!bound) {
                diag_.error(a.loc, "cannot infer the parameter types of this function argument",
                            "pass the other arguments first so their types are known");
                return nullptr;
            }
            Type* want = nullptr;  // null = simpulkan dari block
            if (!te.fn_ret) want = Types::void_();
            else if (!te_mentions(*te.fn_ret, tps) || all_bound(*te.fn_ret, tps, b)) want = resolve_with(*te.fn_ret, b);
            Type* got = nullptr;
            if (a.kind == ExprKind::BlockLit) {
                got = check_block_lit(a, ptypes, want, amp_roots);
                binds[i].block = &a;
            } else if (a.kind == ExprKind::Name) {
                VarInfo* fv = lookup(a.str, a.loc);
                if (fv && fv->type->kind == TypeKind::Fn && fn_) {
                    // meneruskan parameter fungsi: ikut terikat ke fungsi/block yang sama
                    if (!block_ctx_.empty())
                        diag_.error(a.loc, "a block cannot pass on function parameters of the enclosing function yet");
                    for (size_t k = 0; k < fn_->params.size(); k++)
                        if (fn_->params[k].var == fv && k < fn_->bindings.size()) binds[i] = fn_->bindings[k];
                    if (fv->type->elems != ptypes)
                        diag_.error(a.loc, "function '" + fv->name + "' has type " + type_name(fv->type) +
                                               ", expected parameters (" + types_str(ptypes) + ")");
                    got = fv->type->elem;
                    a.type = fv->type;
                    a.var = fv;
                } else if (named(fns_, a.str, a.loc) && !named(fns_, a.str, a.loc)->is_template) {
                    FnDecl* nf = named(fns_, a.str, a.loc);
                    if (nf->param_types != ptypes)
                        diag_.error(a.loc, "function '" + a.str + "' takes (" + types_str(nf->param_types) +
                                               "), expected (" + types_str(ptypes) + ")");
                    binds[i].fn = nf;
                    got = nf->ret;
                    a.type = Types::fn(nf->param_types, nf->ret);
                } else {
                    diag_.error(a.loc, "'" + a.str + "' is not a function", "pass a function name or a block '{ ... }'");
                    return nullptr;
                }
            } else {
                check_expr(a, nullptr);
                diag_.error(a.loc, "argument " + std::to_string(i + 1) + " of '" + tmpl->name +
                                       "' must be a function name or a block '{ ... }'");
                return nullptr;
            }
            if (!got) return nullptr;
            if (want && got != want && !got->is_error())
                diag_.error(a.loc, "the function argument returns " + type_name(got) + ", expected " + type_name(want));
            if (te.fn_ret) unify(*te.fn_ret, got, tps, b);
        }
        std::vector<Type*> targs;
        for (auto& tp : tps) {
            if (!b.count(tp)) {
                diag_.error(e.loc, "cannot infer type parameter '" + tp + "' of '" + tmpl->name + "'",
                            "it must appear in the type of an argument");
                return nullptr;
            }
            targs.push_back(b[tp]);
        }
        return instantiate(tmpl, targs, binds);
    }

    static std::string loc_text(Loc l) {
        std::string f = l.file ? l.file->path : "";
        return (f.empty() ? "" : f + ":") + std::to_string(l.line) + ":" + std::to_string(l.col);
    }

    static std::string types_str(const std::vector<Type*>& ts) {
        std::string s;
        for (size_t i = 0; i < ts.size(); i++) s += (i ? ", " : "") + type_name(ts[i]);
        return s;
    }

    bool all_bound(const TypeExpr& te, const std::vector<std::string>& tps, const std::map<std::string, Type*>& b) {
        if (te.kind == TypeExprKind::Name)
            return std::find(tps.begin(), tps.end(), te.name) == tps.end() || b.count(te.name);
        if (te.kind == TypeExprKind::Fn) {
            for (auto& p : te.fn_params)
                if (!all_bound(p, tps, b)) return false;
            return !te.fn_ret || all_bound(*te.fn_ret, tps, b);
        }
        return !te.elem || all_bound(*te.elem, tps, b);
    }

    // ---------------- trailing block ----------------

    struct BlockCtx {
        size_t base;                 // indeks scope pertama milik block
        Type* ret;                   // null = disimpulkan dari return pertama
        std::set<VarInfo*> captures;
        std::vector<Env> returns;
    };

    // return e from f: hasil untuk pemanggilan aktif terdalam dari f, melewati frame di antaranya.
    // Bahwa f memang selalu aktif di sini dibuktikan nanti lewat graf pemanggilan (compute_unwind).
    void check_return_from(Stmt& s) {
        FnDecl* from_target = named(fns_, s.from_name, s.from_loc);
        if (!from_target || from_target->is_template || !from_target->ret) {
            diag_.error(s.from_loc, !from_target ? "unknown function '" + s.from_name + "'"
                                                     : "cannot 'return ... from' a generic function",
                        "the target must be a named, non-generic function");
            for (auto& e : s.exprs) check_expr(*e, nullptr);
            env_.dead = true;
            return;
        }
        FnDecl* f = from_target;
        s.from_fn =(f == fn_ && block_ctx_.empty()) ? nullptr : f;  // dari fungsi sendiri = return biasa
        Type* ret = f->ret;
        std::vector<Type*> want = ret == Types::void_() ? std::vector<Type*>{}
                                  : ret->kind == TypeKind::Tuple ? ret->elems
                                                                 : std::vector<Type*>{ret};
        returning_ = true;
        if (s.exprs.size() != want.size()) {
            diag_.error(s.loc, "'" + f->name + "' returns " + std::to_string(want.size()) + " value(s), found " +
                                   std::to_string(s.exprs.size()));
            for (auto& e : s.exprs) check_expr(*e, nullptr);
        } else {
            for (size_t i = 0; i < want.size(); i++) {
                Type* t = check_owned(*s.exprs[i], want[i]);
                if (t != want[i] && !t->is_error() && !want[i]->is_error())
                    diag_.error(s.exprs[i]->loc, "expected " + type_name(want[i]) + ", found " + type_name(t));
            }
        }
        returning_ = false;
        env_.dead = true;
    }

    // ---------------- analisis 'return ... from' ----------------

    // Setiap fungsi/block: target 'from' yang mungkin melompat keluar darinya (melewati pemanggilnya).
    // need(n) = target langsung ∪ need(callee) - {n}. Bila need(main) tidak kosong, ada jalur tempat
    // 'return ... from f' terjadi tanpa f aktif → ditolak.
    struct UNode {
        FnDecl* fn = nullptr;
        Expr* block = nullptr;
        std::set<FnDecl*> direct, need;
        std::vector<std::pair<Expr*, int>> calls;  // (ekspresi panggilan, node callee)
    };

    void compute_unwind_entry(FnDecl* f) { compute_unwind(f); }

    // Perkiraan ukuran frame: dua kali total ukuran variabel (temporari & spill) + overhead tetap.
    uint64_t frame_vars(const Block& b) {
        uint64_t n = 0;
        for (auto& st : b.stmts) {
            for (VarInfo* v : st->vars) n += v->is_ref ? 8 : type_size(v->type);
            for (auto& a : st->arms)
                if (a.var) n += a.var->is_ref ? 8 : type_size(a.var->type);
            if (st->body) n += frame_vars(*st->body);
            if (st->else_body) n += frame_vars(*st->else_body);
            for (auto& a : st->arms) n += frame_vars(*a.body);
        }
        return n;
    }

    // Fungsi rekursif wajib #max_depth(N); pemakaian stack native terburuk dari main dan setiap
    // thread diperkirakan lewat graf pemanggilan dan ditolak bila bisa melebihi batas.
    static constexpr uint64_t kStackBudget = 896 * 1024;  // stack default 1 MiB, sisakan untuk runtime/CRT

    void analyze_stack(const std::vector<FnDecl*>& entries) {
        std::vector<UNode> nodes;
        std::map<const void*, int> idx;
        auto add = [&](FnDecl* f, Expr* b) {
            idx[f ? (const void*)f : (const void*)b] = (int)nodes.size();
            UNode n;
            n.fn = f;
            n.block = b;
            nodes.push_back(n);
        };
        for (auto& f : prog_.fns)
            if (f->ret && !f->is_template && !f->is_extern) add(f.get(), nullptr);
        for (auto& f : prog_.instances) add(f.get(), nullptr);
        for (Expr* b : prog_.blocks) add(nullptr, b);
        std::vector<uint64_t> frame(nodes.size());
        for (size_t i = 0; i < nodes.size(); i++) {
            UNode& n = nodes[i];
            const Block* body = n.fn ? n.fn->body.get() : n.block->block.get();
            if (body) unwind_walk_block(*body, n, n.fn, idx);
            uint64_t v = body ? frame_vars(*body) : 0;
            if (n.fn)
                for (auto& prm : n.fn->params) v += prm.var ? (prm.var->is_ref ? 8 : type_size(prm.var->type)) : 0;
            else
                for (VarInfo* bv : n.block->block_vars) v += type_size(bv->type);
            frame[i] = 2 * v + 128;
        }
        // SCC (Tarjan)
        int N = (int)nodes.size(), counter = 0;
        std::vector<int> index(N, -1), low(N, 0), comp(N, -1), stack;
        std::vector<bool> on(N, false);
        int ncomp = 0;
        std::function<void(int)> strong = [&](int v) {
            index[v] = low[v] = counter++;
            stack.push_back(v);
            on[v] = true;
            for (auto& [call, w] : nodes[v].calls) {
                if (index[w] < 0) {
                    strong(w);
                    low[v] = std::min(low[v], low[w]);
                } else if (on[w]) {
                    low[v] = std::min(low[v], index[w]);
                }
            }
            if (low[v] == index[v]) {
                for (;;) {
                    int w = stack.back();
                    stack.pop_back();
                    on[w] = false;
                    comp[w] = ncomp;
                    if (w == v) break;
                }
                ncomp++;
            }
        };
        for (int v = 0; v < N; v++)
            if (index[v] < 0) strong(v);
        std::vector<std::vector<int>> members(ncomp);
        for (int v = 0; v < N; v++) members[comp[v]].push_back(v);
        std::vector<uint64_t> cost(ncomp, 0);
        for (int c = 0; c < ncomp; c++) {
            bool rec = members[c].size() > 1;
            for (int v : members[c])
                for (auto& [call, w] : nodes[v].calls)
                    if (w == v) rec = true;
            uint64_t sum = 0, depth = 1;
            for (int v : members[c]) {
                sum += frame[v];
                FnDecl* f = nodes[v].fn;
                if (!f) continue;
                if (rec && !f->max_depth && !f->origin && !in_prelude(f->loc))
                    diag_.error(f->loc, "'" + f->name + "' is recursive, so its stack use has no compile-time bound",
                                "declare the maximum depth: '#max_depth(N)' on the line before 'fn " + f->name + "'");
                FnDecl* src = f->origin ? f->origin : f;
                if (rec && src->max_depth) depth = std::max(depth, (uint64_t)src->max_depth);
                if (!rec && src->max_depth && !f->origin)
                    diag_.warning(f->loc, "'" + f->name + "' is not recursive; '#max_depth' has no effect");
            }
            cost[c] = rec ? bmul(Big::of_u64(sum), Big::of_u64(depth)).is_fin() ? sum * depth : UINT64_MAX : sum;
        }
        // jalur terberat dalam DAG komponen
        std::vector<uint64_t> best(ncomp, 0);
        std::vector<int> done(ncomp, 0);
        std::function<uint64_t(int)> longest = [&](int c) -> uint64_t {
            if (done[c]) return best[c];
            done[c] = 1;
            uint64_t m = 0;
            for (int v : members[c])
                for (auto& [call, w] : nodes[v].calls)
                    if (comp[w] != c) m = std::max(m, longest(comp[w]));
            best[c] = cost[c] > UINT64_MAX - m ? UINT64_MAX : cost[c] + m;
            return best[c];
        };
        for (FnDecl* e : entries) {
            if (!e || !idx.count(e)) continue;
            uint64_t need = longest(comp[idx[e]]);
            e->stack_need = need;
            if (need > kStackBudget)
                diag_.error(e->loc, "estimated native stack use of '" + e->name + "' is " +
                                        (need == UINT64_MAX ? std::string("unbounded") : std::to_string(need / 1024) + " KiB") +
                                        ", more than the " + std::to_string(kStackBudget / 1024) + " KiB available",
                            "move large values into growable arrays '[..]T', or lower '#max_depth'");
        }
    }

    void compute_unwind(FnDecl* main_fn) {
        std::vector<UNode> nodes;
        std::map<const void*, int> idx;
        auto add = [&](FnDecl* f, Expr* b) {
            idx[f ? (const void*)f : (const void*)b] = (int)nodes.size();
            UNode n;
            n.fn = f;
            n.block = b;
            nodes.push_back(n);
        };
        for (auto& f : prog_.fns)
            if (f->ret && !f->is_template) add(f.get(), nullptr);
        for (auto& f : prog_.instances) add(f.get(), nullptr);
        for (Expr* b : prog_.blocks) add(nullptr, b);
        for (size_t i = 0; i < nodes.size(); i++) {
            UNode& n = nodes[i];
            const Block* body = n.fn ? n.fn->body.get() : n.block->block.get();
            if (body) unwind_walk_block(*body, n, n.fn, idx);
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& n : nodes) {
                std::set<FnDecl*> nw = n.direct;
                for (auto& [call, c] : n.calls)
                    for (FnDecl* t : nodes[c].need)
                        if (t != n.fn) nw.insert(t);
                if (nw != n.need) {
                    n.need = std::move(nw);
                    changed = true;
                }
            }
        }
        int next_id = 1;
        for (auto& n : nodes) {
            // mode panic: setiap panggilan bisa membawa panic ke atas (push/OOM, #max_depth, panic(...))
            for (auto& [call, c] : n.calls) call->may_unwind = panic_mode_ || !nodes[c].need.empty();
            for (FnDecl* t : n.direct)
                if (!t->from_id) t->from_id = next_id++;
        }
        if (!main_fn || !idx.count(main_fn)) return;
        UNode& m = nodes[idx[main_fn]];
        for (auto& [call, c] : m.calls)
            for (FnDecl* t : nodes[c].need)
                if (t != main_fn)
                    diag_.error(call->loc, "this call may 'return ... from " + t->name + "', but no call to '" +
                                               t->name + "' is active here",
                                "'return e from " + t->name + "' is only allowed while a call to '" + t->name +
                                    "' is running further up the call chain");
    }

    void unwind_walk_block(const Block& b, UNode& n, FnDecl* owner, std::map<const void*, int>& idx) {
        for (auto& st : b.stmts) {
            const Stmt& s = *st;
            if (s.kind == StmtKind::Return && s.from_fn) n.direct.insert(s.from_fn);
            for (const ExprPtr* e : {&s.e1, &s.e2, &s.e3})
                if (*e) unwind_walk_expr(**e, n, owner, idx);
            for (auto& e : s.exprs) unwind_walk_expr(*e, n, owner, idx);
            for (auto& e : s.defer_args) unwind_walk_expr(*e, n, owner, idx);
            for (auto& a : s.sel) {
                if (a.op) unwind_walk_expr(*a.op, n, owner, idx);
                if (a.body) unwind_walk_block(*a.body, n, owner, idx);
            }
            if (s.body) unwind_walk_block(*s.body, n, owner, idx);
            if (s.else_body) unwind_walk_block(*s.else_body, n, owner, idx);
            for (auto& a : s.arms) unwind_walk_block(*a.body, n, owner, idx);
        }
    }

    void unwind_walk_expr(Expr& e, UNode& n, FnDecl* owner, std::map<const void*, int>& idx) {
        if (e.kind == ExprKind::BlockLit) return;  // badan block = node tersendiri
        if (e.kind == ExprKind::Call) {
            const void* target = nullptr;
            if (e.var && e.var->type->kind == TypeKind::Fn && owner) {
                for (size_t k = 0; k < owner->params.size(); k++)
                    if (owner->params[k].var == e.var && k < owner->bindings.size())
                        target = owner->bindings[k].block ? (const void*)owner->bindings[k].block
                                                          : (const void*)owner->bindings[k].fn;
            } else if (e.var && e.var->type->kind == TypeKind::FuncRef) {
                for (FnDecl* f : address_taken_)
                    if (Types::funcref(f->param_types, f->ret) == e.var->type && idx.count(f))
                        n.calls.push_back({&e, idx[f]});
            } else if (e.fn) {
                target = e.fn;
            }
            if (target && idx.count(target)) n.calls.push_back({&e, idx[target]});
        }
        for (Expr* c : {e.lhs.get(), e.rhs.get(), e.end.get()})
            if (c) unwind_walk_expr(*c, n, owner, idx);
        for (auto& a : e.args) unwind_walk_expr(*a, n, owner, idx);
    }

    void check_block_return(Stmt& s) {
        BlockCtx& bc = block_ctx_.back();
        Type* t = Types::void_();
        if (s.exprs.size() > 1) diag_.error(s.loc, "a block returns at most one value");
        if (!s.exprs.empty()) t = check_owned(*s.exprs[0], bc.ret && bc.ret != Types::void_() ? bc.ret : nullptr);
        if (!bc.ret) bc.ret = t;
        else if (t != bc.ret && !t->is_error() && !bc.ret->is_error()) {
            Type* want = bc.ret;
            if (!(want->kind == TypeKind::Optional && t == want->elem))
                diag_.error(s.exprs.empty() ? s.loc : s.exprs[0]->loc,
                            "the block returns " + type_name(t) + ", expected " + type_name(want));
        }
        bc.returns.push_back(env_);
        env_.dead = true;
    }

    // Periksa block sebagai badan fungsi yang bisa dipanggil berapa kali pun (seperti badan loop):
    // variabel pemanggil yang diubah block dicarikan invariannya dengan solve_loop.
    Type* check_block_lit(Expr& b, const std::vector<Type*>& ptypes, Type* want, const std::vector<VarInfo*>& amp_roots) {
        if (b.block_id < 0) {
            b.block_id = (int)prog_.blocks.size();
            prog_.blocks.push_back(&b);
        }
        std::vector<std::string> names = b.block_params;
        std::vector<Loc> locs = b.block_param_locs;
        if (names.empty() && ptypes.size() == 1) {
            names = {"it"};
            locs = {b.loc};
        }
        if (names.size() != ptypes.size()) {
            diag_.error(b.loc, "the block takes " + std::to_string(names.size()) + " parameter(s), but " +
                                   std::to_string(ptypes.size()) + " are passed",
                        ptypes.size() > 1 ? "name them: '{ a, b => ... }'" : "");
            return Types::error();
        }
        // ekspresi terakhir = nilai block
        if (want != Types::void_() && !b.block->stmts.empty()) {
            Stmt& last = *b.block->stmts.back();
            if (last.kind == StmtKind::ExprStmt) {
                last.kind = StmtKind::Return;
                last.exprs.push_back(std::move(last.e1));
            }
        }
        Env entry = env_;
        for (VarInfo* r : amp_roots) invalidate_root(r);  // fungsi bisa mengubahnya di antara pemanggilan block
        entry = env_;
        Type* ret = want;
        std::set<VarInfo*> caps;
        auto run = [&]() {
            std::vector<LoopFrame> saved_loops = std::move(loops_);
            loops_.clear();
            scopes_.emplace_back();
            scope_borrows_.emplace_back();
            block_ctx_.push_back({scopes_.size() - 1, want, {}, {}});
            b.block_vars.clear();
            for (size_t i = 0; i < names.size(); i++) {
                VarInfo* v = new_var(names[i], locs[i], ptypes[i], true);
                b.block_vars.push_back(v);
                env_.init.insert(v);
                if (v->name != "_") declare(v);
            }
            check_block(*b.block);
            BlockCtx bc = std::move(block_ctx_.back());
            block_ctx_.pop_back();
            for (VarInfo* r : scope_borrows_.back()) borrowed_[r]--;
            scope_borrows_.pop_back();
            scopes_.pop_back();
            loops_ = std::move(saved_loops);
            Type* r = bc.ret ? bc.ret : Types::void_();
            if (!env_.dead && r != Types::void_())
                diag_.error(b.block->end_loc, "the block may reach its end without producing a value");
            ret = r;
            caps = bc.captures;
            Env be = env_;
            for (Env& x : bc.returns) be = join(be, x);
            be.dead = false;
            return be;
        };
        Env h = solve_loop(entry, *b.block, {}, run, Big::pos_inf(), false);
        env_ = h;
        run();
        env_ = h;  // block bisa berjalan 0 kali atau lebih
        b.captures.assign(caps.begin(), caps.end());
        // urutan tetap (bukan alamat pointer): IR yang sama untuk program yang sama
        std::sort(b.captures.begin(), b.captures.end(), [](VarInfo* x, VarInfo* y) { return x->id < y->id; });
        for (VarInfo* c : b.captures)
            if (c->type->kind == TypeKind::Fn)
                diag_.error(b.loc, "a block cannot use function parameters of the enclosing function yet");
        b.block_ret = ret;
        b.type = Types::fn(ptypes, ret);
        return ret;
    }

    Type* check_struct_lit(Expr& e) {
        StructDecl* sd = nullptr;
        Type* result = nullptr;
        size_t dot = e.str.rfind('.');
        EnumDecl* lit_enum = nullptr;
        if (dot != std::string::npos) lit_enum = named(enums_, e.str.substr(0, dot), e.loc);
        // "Shape.Circle" / "myapp/utils.Shape.Circle" = varian enum; "myapp/utils.Point" = struct
        if (lit_enum || (dot != std::string::npos && e.str.find('/') == std::string::npos)) {
            std::string en = e.str.substr(0, dot), vn = e.str.substr(dot + 1);
            int idx = !lit_enum ? -1 : lit_enum->find_variant(vn);
            if (idx < 0) {
                diag_.error(e.loc, !lit_enum ? "unknown enum '" + en + "'"
                                             : "enum '" + en + "' has no variant '" + vn + "'");
                for (auto& a : e.args) check_expr(*a, nullptr);
                return Types::error();
            }
            sd = lit_enum->variants[idx].payload;
            if (!sd) {
                diag_.error(e.loc, "variant '" + vn + "' has no fields", "write '" + en + "." + vn + "'");
                return Types::error();
            }
            e.variant = idx;
            result = Types::enum_(lit_enum);
        } else {
            sd = named(structs_, e.str, e.loc);
            if (!sd) {
                diag_.error(e.loc, "unknown struct '" + e.str + "'");
                for (auto& a : e.args) check_expr(*a, nullptr);
                return Types::error();
            }
        }
        if (foreign(sd->loc, e.loc)) {
            // literal tipe dari package lain: setiap field yang diisi harus diekspor
            if (e.field_names.empty()) {
                for (auto& f : sd->fields)
                    if (!exported(f.name)) {
                        diag_.error(e.loc, "cannot build '" + sd->name + "' here: field '" + f.name +
                                               "' is not exported",
                                    "use a constructor function from its package, e.g. New" + f.name);
                        break;
                    }
            } else {
                for (size_t i = 0; i < e.field_names.size(); i++)
                    check_visible(sd->loc, e.field_locs[i], "field", e.field_names[i], sd->name);
            }
        }
        size_t n = sd->fields.size();
        e.field_order.assign(e.args.size(), -1);
        if (e.field_names.empty()) {
            if (e.args.size() != n)
                diag_.error(e.loc, "'" + sd->name + "' has " + std::to_string(n) + " field(s), found " +
                                       std::to_string(e.args.size()) + " value(s)");
            for (size_t i = 0; i < e.args.size(); i++)
                if (i < n) e.field_order[i] = (int)i;
        } else {
            std::vector<bool> seen(n, false);
            for (size_t i = 0; i < e.args.size(); i++) {
                int idx = sd->find_field(e.field_names[i]);
                if (idx < 0) {
                    diag_.error(e.field_locs[i], "struct '" + sd->name + "' has no field '" + e.field_names[i] + "'");
                } else if (seen[idx]) {
                    diag_.error(e.field_locs[i], "field '" + e.field_names[i] + "' given twice");
                } else {
                    seen[idx] = true;
                    e.field_order[i] = idx;
                }
            }
            std::string missing;
            for (size_t i = 0; i < n; i++)
                if (!seen[i]) missing += (missing.empty() ? "" : ", ") + sd->fields[i].name;
            if (!missing.empty())
                diag_.error(e.loc, "missing field(s) in '" + sd->name + "' literal: " + missing,
                            "every field must be initialized");
        }
        for (size_t i = 0; i < e.args.size(); i++) {
            int idx = e.field_order[i];
            Type* ft = idx >= 0 ? sd->fields[idx].type : nullptr;
            Type* at = ft && contains_dyn(ft) ? check_owned(*e.args[i], ft) : check_expr(*e.args[i], ft);
            if (ft && at != ft && !at->is_error() && !ft->is_error())
                diag_.error(e.args[i]->loc, "field '" + sd->fields[idx].name + "': expected " + type_name(ft) +
                                                ", found " + type_name(at));
        }
        return result ? result : Types::struct_(sd);
    }

    static bool lossless_int(const Type* s, const Type* d) {
        if (s->is_signed == d->is_signed) return d->bits >= s->bits;
        return !s->is_signed && d->is_signed && d->bits > s->bits;
    }

    Type* check_cast(Expr& e) {
        Type* dst = resolve_type(e.cast_type);
        // literal tanpa tipe langsung bertipe tujuan: '-128 as i8', '255 as u8'
        bool lit_to_dst = is_untyped_lit(*e.lhs) && dst->is_numeric() && e.cast_kind != CastKind::Opt;
        Type* src = check_expr(*e.lhs, lit_to_dst ? dst : nullptr);
        if (src->is_error() || dst->is_error()) return dst;
        const char* kw = e.cast_kind == CastKind::Plain     ? "as"
                         : e.cast_kind == CastKind::Checked ? "as!"
                         : e.cast_kind == CastKind::Wrap    ? "as%"
                         : e.cast_kind == CastKind::Opt     ? "as?"
                                                            : "as|";
        auto bad = [&](const std::string& help) {
            diag_.error(e.loc, "cannot convert " + type_name(src) + " to " + type_name(dst) + " with '" + kw + "'",
                        help);
            return dst;
        };
        bool src_num = src->is_numeric() || src == Types::bool_();
        if (!src_num || !dst->is_numeric()) return bad("only numbers (and bool → integer) can be converted");
        Interval r = range(*e.lhs);
        if (src == Types::bool_()) {
            if (!dst->is_int()) return bad("convert bool to an integer type");
            set_range(e, Interval::of(Big::of_u64(0), Big::of_u64(1)));
            return dst;
        }
        switch (e.cast_kind) {
            case CastKind::Plain:
                if (src->is_int() && dst->is_int() && !lossless_int(src, dst))
                    return bad("'as' only allows lossless conversions; use 'as!' (requires proof that the value "
                               "fits), 'as%' (wrap/truncate) or 'as|' (saturate)");
                if (src->is_float() && dst->is_int())
                    return bad("float to integer must use 'as|' (saturating; NaN becomes 0)");
                if (dst->is_int()) set_range(e, r);
                return dst;
            case CastKind::Checked:
                if (src->is_float() && dst->is_int())
                    return bad("float to integer must use 'as|' (saturating; NaN becomes 0)");
                if (src->is_int() && dst->is_int() && !fits(r, dst)) {
                    Interval tr = type_range(dst);
                    diag_.error(e.loc, "cannot prove '" + expr_str(*e.lhs) + "' (range " + r.str() + ") fits in " +
                                           type_name(dst) + " [" + tr.lo.str() + ", " + tr.hi.str() + "]",
                                "check the range first (e.g. 'if x > " + tr.hi.str() +
                                    " { return ... }'), or use 'as%' (wrap) / 'as|' (saturate)");
                    r = clamp_to(r, dst);
                }
                if (dst->is_int()) set_range(e, r);
                return dst;
            case CastKind::Wrap:
                if (!src->is_int() || !dst->is_int()) return bad("'as%' converts between integer types only");
                set_range(e, fits(r, dst) ? r : type_range(dst));
                return dst;
            case CastKind::Opt:
                if (!dst->is_int()) return bad("'as?' converts to an integer type");
                return Types::optional(dst);  // none bila tidak muat (atau NaN)
            case CastKind::Sat:
                if (!dst->is_int()) return bad("'as|' converts to an integer type");
                set_range(e, src->is_int() ? clamp_to(r, dst) : type_range(dst));
                return dst;
        }
        return dst;
    }

    Program& prog_;
    Diag& diag_;
    std::unordered_map<std::string, StructDecl*> structs_;
    std::unordered_map<std::string, FnDecl*> fns_;
    std::unordered_map<const void*, std::unordered_map<std::string, FnDecl*>> methods_;
    std::unordered_map<std::string, EnumDecl*> enums_;
    std::vector<TParamScope> tparams_;
    bool lazy_struct_ = false;
    std::map<std::string, FnDecl*> inst_memo_;
    std::vector<FnDecl*> pending_;
    std::vector<BlockCtx> block_ctx_;
    std::set<Type*> send_types_;
    std::map<const Expr*, std::pair<Type*, Loc>> recv_sites_;
    std::map<const Expr*, std::pair<Loc, std::string>> unsafe_calls_, panics_;
    std::set<FnDecl*> run_targets_;
    Expr* owned_recv_ok_ = nullptr;
    std::vector<Env> last_breaks_;
    std::unordered_map<std::string, VarInfo*> consts_;
    std::vector<std::unordered_map<std::string, VarInfo*>> scopes_;
    std::unordered_map<const Expr*, Interval> ranges_;
    std::unordered_map<const Stmt*, Big> trip_;
    Expr* owned_ok_ = nullptr;  // ekspresi yang sedang berada di posisi kepemilikan
    std::map<VarInfo*, int> borrowed_;               // root → jumlah pinjaman aktif
    std::vector<std::vector<VarInfo*>> scope_borrows_;  // pinjaman yang berakhir di akhir scope
    std::unordered_map<const Expr*, Interval> grow_by_;  // append: rentang jumlah elemen yang ditambah
    std::set<Path> last_induct_;
    bool returning_ = false;
    bool panic_mode_ = false;
    std::set<FnDecl*> address_taken_;  // fungsi yang dipakai sebagai nilai func
    bool in_defer_ = false;
    size_t defer_scope_base_ = 0;
    size_t defer_block_base_ = 0;
    FnDecl* fn_ = nullptr;
    Env env_;
    std::vector<LoopFrame> loops_;
    int next_var_id_ = 0;
};

}  // namespace

SemaResult check_program(Program& prog, Diag& diag) {
    Checker c(prog, diag);
    return c.run();
}

}  // namespace tl
