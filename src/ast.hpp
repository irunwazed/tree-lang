#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "diag.hpp"
#include "lexer.hpp"
#include "types.hpp"

namespace tl {

struct FnDecl;
struct StructDecl;

// Ekspresi tipe di sumber: i64, Point, [4]i64, [N]Point, [..]u8, []i64.
enum class TypeExprKind { Name, Array, Dyn, Slice, Ref, Optional, Fn };

struct TypeExpr {
    Loc loc;
    TypeExprKind kind = TypeExprKind::Name;
    std::string name;                 // Name
    uint64_t size_val = 0;            // Array: ukuran literal
    std::string size_name;            // Array: ukuran dari konstanta (jika bukan literal)
    std::shared_ptr<TypeExpr> elem;   // Array / Dyn / Slice / Ref / Optional
    std::vector<TypeExpr> fn_params;  // Fn: fn(A, B) R
    std::vector<TypeExpr> targs;      // Name: Pool[T], Handle[T], Link[T]
    std::shared_ptr<TypeExpr> fn_ret; // Fn: null = tanpa hasil
    std::string str() const {
        switch (kind) {
            case TypeExprKind::Name:
                if (!targs.empty()) {
                    std::string s = name + "[";
                    for (size_t i = 0; i < targs.size(); i++) s += (i ? ", " : "") + targs[i].str();
                    return s + "]";
                }
                return name;
            case TypeExprKind::Fn: {
                std::string s = "fn(";
                for (size_t i = 0; i < fn_params.size(); i++) s += (i ? ", " : "") + fn_params[i].str();
                return s + ")" + (fn_ret ? " " + fn_ret->str() : "");
            }
            case TypeExprKind::Array:
                return "[" + (size_name.empty() ? std::to_string(size_val) : size_name) + "]" + elem->str();
            case TypeExprKind::Dyn: return "[..]" + elem->str();
            case TypeExprKind::Slice: return "[]" + elem->str();
            case TypeExprKind::Ref: return "&" + elem->str();
            case TypeExprKind::Optional: return elem->str() + "?";
        }
        return name;
    }
};

// Informasi variabel/parameter/konstanta hasil resolusi nama.
struct VarInfo {
    std::string name;
    Loc loc;
    Type* type = nullptr;
    bool is_mutable = false;
    bool is_param = false;
    bool is_const = false;
    // referensi (parameter '&T' atau 'r := &x'): 'type' = tipe target, akses lewat auto-deref
    bool is_ref = false;
    VarInfo* ref_root = nullptr;  // variabel pemilik memori yang ditunjuk (lokal 'r := &x')
    // nilai konstanta (int): tanda + magnitudo; atau float/bool
    bool const_neg = false;
    uint64_t const_mag = 0;
    double const_f = 0;
    bool const_b = false;
    int id = 0;
};

enum class ExprKind {
    IntLit, FloatLit, BoolLit, StrLit, Name, Unary, Binary, Call, Field, StructLit, Cast,
    NoneLit,   // none
    BlockLit,  // trailing block: f(xs) { a, b => ... }   /   { it * 2 }
    Spawn,     // run worker(args)   (str = nama fungsi; goroutine baru)
    ChanMake,  // chan[T](cap)       (array_type = T, args[0] = kapasitas)
    ChanSend,  // ch <- v            (lhs = ch, rhs = v) → bool
    ChanRecv,  // <-ch               (lhs = ch) → T?
    Index,     // a[i]            (lhs, rhs)
    SliceOf,   // a[i..j]         (lhs, rhs = awal atau null, end = akhir atau null)
    ArrayLit,  // [3]i64{1, 2, 3}  /  [..]i64{1, 2}   (array_type, args)
};

enum class CastKind { Plain, Checked, Wrap, Sat, Opt };  // as, as!, as%, as|, as?

enum class Builtin {
    None, Print, Abort, Len, Copy, Push, Append, Get,
    PoolAdd, PoolGet, PoolSet, PoolRemove, PoolContains, PoolCount,  // Pool[T]
    Link, PushLink, At, Put,                                         // Link[T] pada array
    ToBytes, FromBytes,                                              // serialisasi tervalidasi
    Send, Recv, TryRecv, Join,                                       // (lama, M8)
    Close, Wait,                                                     // close(ch) / wait(task)
    Str,                                                             // str(a, b, ...) -> [..]u8
};

struct Expr {
    ExprKind kind;
    Loc loc;
    Type* type = nullptr;  // diisi sema

    // literal
    uint64_t int_val = 0;
    bool int_neg = false;  // literal negatif hasil folding '-' + literal
    double float_val = 0;
    bool bool_val = false;
    std::string str;  // Name / StrLit / Field / Call (nama fungsi) / StructLit (nama struct)

    // operator
    Tok op = Tok::Eof;
    std::unique_ptr<Expr> lhs, rhs;  // Unary: lhs; Binary: lhs, rhs; Field/Cast: lhs
    std::unique_ptr<Expr> end;       // SliceOf
    std::vector<std::unique_ptr<Expr>> args;  // Call / StructLit
    std::vector<std::string> field_names;     // StructLit dengan nama (kosong = posisional)
    std::vector<Loc> field_locs;
    bool method_call = false;                 // x.f(a) → f(x, a)

    TypeExpr cast_type;
    CastKind cast_kind = CastKind::Plain;
    TypeExpr array_type;  // ArrayLit

    // hasil sema
    VarInfo* var = nullptr;
    FnDecl* fn = nullptr;
    Builtin builtin = Builtin::None;
    int field_index = -1;
    std::vector<int> field_order;  // StructLit: indeks field untuk tiap argumen
    bool str_as_slice = false;     // StrLit diteruskan sebagai []u8 (tanpa salinan)
    bool borrow = false;           // argumen dikirim sebagai referensi (&x, reborrow, receiver &T)
    int variant = -1;              // literal enum: indeks varian (StructLit "E.V{...}" / Field "E.V")
    // BlockLit
    std::vector<std::string> block_params;
    std::vector<Loc> block_param_locs;
    std::shared_ptr<struct Block> block;
    std::vector<VarInfo*> block_vars;  // sema: parameter block
    std::vector<VarInfo*> captures;    // sema: variabel pemanggil yang dipakai block
    Type* block_ret = nullptr;         // sema
    int block_id = -1;                 // sema
    bool wrap_opt = false;         // nilai T dibungkus menjadi T? (some)
    bool may_unwind = false;       // sema: panggilan ini bisa berakhir dengan 'return ... from' ke atas
    bool move_src = false;         // sema: nilai ini memindahkan variabel pemiliknya (ch <- xs)

    Expr(ExprKind k, Loc l) : kind(k), loc(l) {}
};

using ExprPtr = std::unique_ptr<Expr>;

struct Block;
using BlockPtr = std::unique_ptr<Block>;

// Satu cabang select.
struct SelectArm {
    enum Kind { Recv, Send, Timeout, Default };
    Kind kind = Recv;
    Loc loc;
    std::string bind;      // x := <-ch   /   ok := ch <- v
    Loc bind_loc;
    ExprPtr op;  // ChanRecv / ChanSend / argumen timeout
    BlockPtr body;
    VarInfo* var = nullptr;           // sema
};

// Satu cabang match.
struct MatchArm {
    enum Kind { Wild, Variant, Some, None, Values };
    Kind kind = Wild;
    Loc loc;
    std::string name;      // Variant
    std::string bind;      // nama binding payload (kosong = tanpa binding)
    Loc bind_loc;
    bool bind_ref = false;  // Variant(&r) / some(&r)
    std::vector<ExprPtr> values;  // Values: literal integer/bool
    BlockPtr body;
    // sema
    int variant = -1;
    VarInfo* var = nullptr;
};


enum class StmtKind {
    Let,       // a := e;  a, b := f();  var a T;  var a T = e;  var a = e
    Assign,    // target = e;  target op= e;
    ExprStmt,  // e;
    Discard,   // _ = e;
    If,
    While,     // for cond { }
    Loop,      // for { }
    For,       // for i := range [a..]b { }   /   for i, x := range xs { }
    Break,
    Continue,
    Return,
    Block,
    Match,     // match e { Pola { ... } ... }
    Select,    // select { x := <-a { } ok := b <- v { } timeout(ms) { } default { } }
};



struct Stmt {
    StmtKind kind;
    Loc loc;

    // Let
    bool is_var = false;      // ditulis dengan 'var'
    bool zero_init = false;   // 'var x T' tanpa nilai: diisi nilai nol
    std::vector<std::string> names;
    std::vector<Loc> name_locs;
    bool has_type = false;
    TypeExpr type;
    std::vector<VarInfo*> vars;  // sema (For: [indeks, elemen?])
    bool range_array = false;    // sema: For melintasi array (bukan hitungan)

    // umum
    ExprPtr e1, e2, e3;  // Let: e1=init; Assign: e1=target,e2=value; If/While: e1=cond;
                         // For: e1=start (boleh null), e2=end
    Tok op = Tok::Eq;    // Assign: Eq atau PlusEq dst.
    std::vector<ExprPtr> exprs;  // Return
    BlockPtr body;               // If-then / While / Loop / For / Block
    BlockPtr else_body;          // If: else { } (else-if dibungkus jadi block berisi satu If)
    bool if_some = false;        // if some x := opt { }
    // Return: 'return e from f' — kembali dari pemanggilan aktif terdalam dari f
    std::string from_name;
    Loc from_loc;
    FnDecl* from_fn = nullptr;   // sema
    std::vector<MatchArm> arms;  // Match
    std::vector<SelectArm> sel;  // Select
    bool range_chan = false;     // sema: for x := range ch

    Stmt(StmtKind k, Loc l) : kind(k), loc(l) {}
};

using StmtPtr = std::unique_ptr<Stmt>;

struct Block {
    Loc loc;
    Loc end_loc;
    std::vector<StmtPtr> stmts;
};

struct Param {
    std::string name;
    Loc loc;
    TypeExpr type;
    VarInfo* var = nullptr;
};

// Fungsi yang terikat ke parameter fungsi pada sebuah instansiasi.
struct FnBinding {
    Expr* block = nullptr;   // trailing block di pemanggil
    FnDecl* fn = nullptr;    // fungsi bernama
};

struct FnDecl {
    std::string name;
    Loc loc;
    std::vector<std::string> type_params;  // fn max[T](a, b T) T
    bool is_thread = false;                // thread fn worker(...) { }
    bool is_extern = false;                // unsafe extern fn name(...) R   (simbol C, tanpa badan)
    std::string c_name;                    // extern di package lain: nama simbol C asli (name di-mangle)
    uint64_t stack_need = 0;               // sema: perkiraan stack native terburuk bila jadi titik masuk
    int64_t max_depth = 0;                 // #max_depth(N): batas rekursi (0 = tidak rekursif)
    // sema: template = generik atau punya parameter fungsi; hanya instansiasinya yang dikompilasi
    bool is_template = false;
    FnDecl* origin = nullptr;
    std::vector<Type*> type_args;
    std::vector<FnBinding> bindings;  // per parameter (hanya untuk parameter bertipe fungsi)
    std::string inst_name;
    int from_id = 0;  // sema: > 0 bila ada 'return ... from <fungsi ini>' 
    // method: fn (p Point) area() i64  →  receiver = p, receiver_type = Point
    bool has_receiver = false;
    Param receiver;
    StructDecl* receiver_struct = nullptr;  // sema
    std::string receiver_type_name;         // sema: nama tipe receiver (struct atau enum)
    std::vector<Param> params;
    std::vector<TypeExpr> ret_types;
    BlockPtr body;
    // sema
    Type* ret = nullptr;  // void, tunggal, atau tuple
    std::vector<Type*> param_types;
};

struct FieldDecl {
    std::string name;
    Loc loc;
    TypeExpr type_expr;
    Type* type = nullptr;
};

struct StructDecl {
    std::string name;
    Loc loc;
    std::vector<FieldDecl> fields;
    Type* type = nullptr;
    bool resolving = false;
    bool resolved = false;
    int find_field(const std::string& n) const {
        for (size_t i = 0; i < fields.size(); i++)
            if (fields[i].name == n) return (int)i;
        return -1;
    }
};

struct VariantDecl {
    std::string name;
    Loc loc;
    bool has_payload = false;
    std::vector<FieldDecl> fields;
    StructDecl* payload = nullptr;  // sema: struct sintetis "E.V" untuk field payload
};

struct EnumDecl {
    std::string name;
    Loc loc;
    std::vector<VariantDecl> variants;
    Type* type = nullptr;
    int find_variant(const std::string& n) const {
        for (size_t i = 0; i < variants.size(); i++)
            if (variants[i].name == n) return (int)i;
        return -1;
    }
};

struct ConstDecl {
    std::string name;
    Loc loc;
    bool has_type = false;
    TypeExpr type;
    ExprPtr value;
    VarInfo* var = nullptr;
};

// import "myapp/utils"   /   import u "myapp/utils"
struct ImportDecl {
    std::string alias;  // nama lokal (kosong = segmen terakhir path)
    std::string path;
    Loc loc;
};

struct Program {
    // klausa 'package nama' (kosong = file tunggal tanpa package) dan import file ini
    std::string package_name;
    Loc package_loc;
    std::vector<ImportDecl> imports;
    std::vector<std::unique_ptr<StructDecl>> structs;
    std::vector<std::unique_ptr<EnumDecl>> enums;
    std::vector<std::unique_ptr<FnDecl>> fns;
    std::vector<std::unique_ptr<ConstDecl>> consts;
    std::vector<std::unique_ptr<VarInfo>> vars;  // pemilik semua VarInfo
    std::vector<std::unique_ptr<FnDecl>> instances;  // sema: instansiasi template
    std::vector<Expr*> blocks;                        // sema: semua trailing block yang dipakai
};

}  // namespace tl
