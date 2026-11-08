#include "types.hpp"

#include "ast.hpp"

namespace tl {

namespace {

Type* make(TypeKind k) {
    static std::vector<std::unique_ptr<Type>> pool;
    pool.push_back(std::make_unique<Type>());
    pool.back()->kind = k;
    return pool.back().get();
}

}  // namespace

Type* Types::error() {
    static Type* t = make(TypeKind::Error);
    return t;
}

Type* Types::void_() {
    static Type* t = make(TypeKind::Void);
    return t;
}

Type* Types::bool_() {
    static Type* t = make(TypeKind::Bool);
    return t;
}

Type* Types::thread_() {
    static Type* t = make(TypeKind::Thread);
    return t;
}

Type* Types::str_lit() {
    static Type* t = make(TypeKind::StrLit);
    return t;
}

Type* Types::int_(int bits, bool is_signed) {
    static Type* cache[2][4] = {};
    int idx = bits == 8 ? 0 : bits == 16 ? 1 : bits == 32 ? 2 : 3;
    Type*& slot = cache[is_signed ? 1 : 0][idx];
    if (!slot) {
        slot = make(TypeKind::Int);
        slot->bits = bits;
        slot->is_signed = is_signed;
    }
    return slot;
}

Type* Types::float_(int bits) {
    static Type* f32 = nullptr;
    static Type* f64 = nullptr;
    Type*& slot = bits == 32 ? f32 : f64;
    if (!slot) {
        slot = make(TypeKind::Float);
        slot->bits = bits;
    }
    return slot;
}

Type* Types::struct_(StructDecl* sd) {
    if (!sd->type) {
        sd->type = make(TypeKind::Struct);
        sd->type->sd = sd;
    }
    return sd->type;
}

Type* Types::tuple(const std::vector<Type*>& elems) {
    static std::vector<Type*> tuples;
    for (Type* t : tuples)
        if (t->elems == elems) return t;
    Type* t = make(TypeKind::Tuple);
    t->elems = elems;
    tuples.push_back(t);
    return t;
}

Type* Types::array(Type* elem, uint64_t count) {
    static std::vector<Type*> cache;
    for (Type* t : cache)
        if (t->elem == elem && t->count == count) return t;
    Type* t = make(TypeKind::Array);
    t->elem = elem;
    t->count = count;
    cache.push_back(t);
    return t;
}

Type* Types::dyn(Type* elem) {
    static std::vector<Type*> cache;
    for (Type* t : cache)
        if (t->elem == elem) return t;
    Type* t = make(TypeKind::Dyn);
    t->elem = elem;
    cache.push_back(t);
    return t;
}

Type* Types::slice(Type* elem) {
    static std::vector<Type*> cache;
    for (Type* t : cache)
        if (t->elem == elem) return t;
    Type* t = make(TypeKind::Slice);
    t->elem = elem;
    cache.push_back(t);
    return t;
}

Type* Types::enum_(EnumDecl* ed) {
    if (!ed->type) {
        ed->type = make(TypeKind::Enum);
        ed->type->ed = ed;
    }
    return ed->type;
}

Type* Types::fn(const std::vector<Type*>& params, Type* ret) {
    static std::vector<Type*> cache;
    for (Type* t : cache)
        if (t->elems == params && t->elem == ret) return t;
    Type* t = make(TypeKind::Fn);
    t->elems = params;
    t->elem = ret;
    cache.push_back(t);
    return t;
}

Type* Types::funcref(const std::vector<Type*>& params, Type* ret) {
    static std::vector<Type*> cache;
    for (Type* t : cache)
        if (t->elems == params && t->elem == ret) return t;
    Type* t = make(TypeKind::FuncRef);
    t->elems = params;
    t->elem = ret;
    cache.push_back(t);
    return t;
}

Type* Types::wrap(TypeKind k, Type* elem) {
    static std::vector<Type*> cache;
    for (Type* t : cache)
        if (t->kind == k && t->elem == elem) return t;
    Type* t = make(k);
    t->elem = elem;
    cache.push_back(t);
    return t;
}

Type* Types::optional(Type* elem) {
    static std::vector<Type*> cache;
    for (Type* t : cache)
        if (t->elem == elem) return t;
    Type* t = make(TypeKind::Optional);
    t->elem = elem;
    cache.push_back(t);
    return t;
}

uint64_t enum_payload_words(const EnumDecl* ed) {
    uint64_t words = 0;
    for (auto& v : ed->variants)
        if (v.payload && v.payload->type) {
            uint64_t n = (type_size(v.payload->type) + 7) / 8;
            if (n > words) words = n;
        }
    return words;
}

Type* Types::ref(Type* elem) {
    static std::vector<Type*> cache;
    for (Type* t : cache)
        if (t->elem == elem) return t;
    Type* t = make(TypeKind::Ref);
    t->elem = elem;
    cache.push_back(t);
    return t;
}

uint64_t type_size(const Type* t) {
    switch (t->kind) {
        case TypeKind::Bool: return 1;
        case TypeKind::Int:
        case TypeKind::Float: return (uint64_t)t->bits / 8;
        case TypeKind::Array: return t->count * type_size(t->elem);
        case TypeKind::Dyn: return 24;
        case TypeKind::Slice: return 16;
        case TypeKind::Ref: return 8;
        case TypeKind::Fn: return 8;
        case TypeKind::FuncRef: return 8;
        case TypeKind::Pool: return 72;
        case TypeKind::Handle: return 8;
        case TypeKind::Link: return 4;
        case TypeKind::Thread: return 8;
        case TypeKind::Chan: return 8;
        case TypeKind::Enum: return 8 + 8 * enum_payload_words(t->ed);
        case TypeKind::Optional: return type_size(t->elem) + 8;
        case TypeKind::Struct: {
            uint64_t n = 0;
            for (auto& f : t->sd->fields)
                if (f.type) n += type_size(f.type) + 7;  // perkiraan atas dengan padding
            return n;
        }
        case TypeKind::Tuple: {
            uint64_t n = 0;
            for (Type* e : t->elems) n += type_size(e) + 7;
            return n;
        }
        default: return 0;
    }
}

bool contains_dyn(const Type* t) {
    switch (t->kind) {
        case TypeKind::Dyn:
        case TypeKind::Pool: return true;
        case TypeKind::Array: return contains_dyn(t->elem);
        case TypeKind::Struct:
            for (auto& f : t->sd->fields)
                if (f.type && contains_dyn(f.type)) return true;
            return false;
        default: return false;
    }
}

Type* Types::primitive(const std::string& n) {
    if (n == "i8") return int_(8, true);
    if (n == "i16") return int_(16, true);
    if (n == "i32") return int_(32, true);
    if (n == "i64") return int_(64, true);
    if (n == "u8") return int_(8, false);
    if (n == "u16") return int_(16, false);
    if (n == "u32") return int_(32, false);
    if (n == "u64") return int_(64, false);
    if (n == "f32") return float_(32);
    if (n == "f64") return float_(64);
    if (n == "bool") return bool_();
    return nullptr;
}

std::string type_name(const Type* t) {
    switch (t->kind) {
        case TypeKind::Error: return "<error>";
        case TypeKind::Void: return "void";
        case TypeKind::Bool: return "bool";
        case TypeKind::Int: return std::string(t->is_signed ? "i" : "u") + std::to_string(t->bits);
        case TypeKind::Float: return "f" + std::to_string(t->bits);
        case TypeKind::StrLit: return "string literal";
        case TypeKind::Struct: return t->sd->name;
        case TypeKind::Array: return "[" + std::to_string(t->count) + "]" + type_name(t->elem);
        case TypeKind::Dyn: return "[..]" + type_name(t->elem);
        case TypeKind::Slice: return "[]" + type_name(t->elem);
        case TypeKind::Ref: return "&" + type_name(t->elem);
        case TypeKind::Enum: return t->ed->name;
        case TypeKind::Optional: return type_name(t->elem) + "?";
        case TypeKind::Pool: return "Pool[" + type_name(t->elem) + "]";
        case TypeKind::Handle: return "Handle[" + type_name(t->elem) + "]";
        case TypeKind::Link: return "Link[" + type_name(t->elem) + "]";
        case TypeKind::Thread: return "Task";
        case TypeKind::Chan: return "Chan[" + type_name(t->elem) + "]";
        case TypeKind::FuncRef: {
            std::string s = "func(";
            for (size_t i = 0; i < t->elems.size(); i++) s += (i ? ", " : "") + type_name(t->elems[i]);
            return s + ")" + (t->elem && t->elem != Types::void_() ? " " + type_name(t->elem) : "");
        }
        case TypeKind::Fn: {
            std::string s = "fn(";
            for (size_t i = 0; i < t->elems.size(); i++) s += (i ? ", " : "") + type_name(t->elems[i]);
            return s + ")" + (t->elem && t->elem != Types::void_() ? " " + type_name(t->elem) : "");
        }
        case TypeKind::Tuple: {
            std::string s;
            for (size_t i = 0; i < t->elems.size(); i++) {
                if (i) s += ", ";
                s += type_name(t->elems[i]);
            }
            return "(" + s + ")";
        }
    }
    return "?";
}

}  // namespace tl
