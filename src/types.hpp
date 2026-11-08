#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tl {

struct StructDecl;
struct EnumDecl;

enum class TypeKind {
    Error,   // hasil dari ekspresi yang sudah dilaporkan error; menekan error lanjutan
    Void,
    Bool,
    Int,
    Float,
    Struct,
    Tuple,   // hasil multi-return: `-> i64, bool`
    StrLit,  // literal string: menjadi [..]u8 saat disimpan, atau []u8 saat diteruskan
    Array,   // [N]T  : array ukuran tetap, nilai inline
    Dyn,     // [..]T : array grow-only di region data stack miliknya sendiri
    Slice,   // []T   : tampilan baca-saja {ptr, len}; parameter dan argumen sementara
    Ref,     // &T    : referensi (parameter / 'r := &x'), dipakai dengan auto-deref
    Enum,    // type E enum { A { ... }  B }   : tag + payload (ukuran varian terbesar)
    Optional,  // T?  : ada nilai (some) atau tidak (none); tanpa null pointer
    Fn,      // fn(A, B) R : parameter fungsi (tidak bisa escape); elems = parameter, elem = hasil
    Pool,    // Pool[T]   : slot dengan generasi (memiliki region data)
    Handle,  // Handle[T] : { indeks u32, generasi u32 } ke sebuah Pool[T]
    Link,    // Link[T]   : offset u32 bertipe, relatif terhadap awal array [..]T
    Thread,  // Task: hasil 'run f(...)', ditunggu dengan wait()
    FuncRef, // func(A, B) R : referensi ke fungsi top-level bernama (tanpa capture): boleh disimpan & dikirim
    Chan,    // Chan[T]: handle channel (satu-satunya hal yang dibagi antar goroutine; disinkronkan runtime)
};

struct Type {
    TypeKind kind = TypeKind::Error;
    int bits = 0;
    bool is_signed = false;
    StructDecl* sd = nullptr;
    EnumDecl* ed = nullptr;
    std::vector<Type*> elems;
    Type* elem = nullptr;  // Array / Dyn / Slice
    uint64_t count = 0;    // Array

    bool is_array_like() const { return kind == TypeKind::Array || kind == TypeKind::Dyn || kind == TypeKind::Slice; }

    bool is_int() const { return kind == TypeKind::Int; }
    bool is_float() const { return kind == TypeKind::Float; }
    bool is_numeric() const { return is_int() || is_float(); }
    bool is_error() const { return kind == TypeKind::Error; }
};

// Semua tipe di-intern, sehingga kesamaan tipe = kesamaan pointer.
class Types {
public:
    static Type* error();
    static Type* void_();
    static Type* bool_();
    static Type* int_(int bits, bool is_signed);
    static Type* float_(int bits);
    static Type* str_lit();
    static Type* struct_(StructDecl* sd);
    static Type* tuple(const std::vector<Type*>& elems);
    static Type* array(Type* elem, uint64_t count);
    static Type* dyn(Type* elem);
    static Type* slice(Type* elem);
    static Type* ref(Type* elem);
    static Type* enum_(EnumDecl* ed);
    static Type* optional(Type* elem);
    static Type* fn(const std::vector<Type*>& params, Type* ret);
    static Type* funcref(const std::vector<Type*>& params, Type* ret);
    static Type* wrap(TypeKind k, Type* elem);  // Pool / Handle / Link
    static Type* thread_();
    static Type* i64() { return int_(64, true); }
    // Nama tipe primitif ("i32", "f64", "bool") → tipe, atau nullptr.
    static Type* primitive(const std::string& name);
};

std::string type_name(const Type* t);

// Ukuran tetap (byte) untuk nilai inline; Dyn/Slice = ukuran header-nya.
uint64_t type_size(const Type* t);
// Tipe berisi Dyn (array tumbuh) di dalamnya (struct/array/tuple).
bool contains_dyn(const Type* t);
// Jumlah word 8-byte untuk payload enum (varian terbesar).
uint64_t enum_payload_words(const EnumDecl* ed);

}  // namespace tl
