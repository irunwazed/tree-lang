// Parser tree-lang: sintaks sederhana ala Go.
//
//   type Point struct { x, y i64 }
//   const N = 10
//   fn (p Point) area() i64 { return p.x * p.y }
//   fn divmod(a, b u32) (u32, u32) { ... }
//   x := 1      var y i64      for i := range 10 { }      for c { }      for { }
//
// Statement diakhiri ';' yang disisipkan lexer di akhir baris (aturan Go).

#include "parser.hpp"

namespace tl {

namespace {

// Tingkat presedensi operator biner (lebih besar = lebih kuat).
int binary_prec(Tok t) {
    switch (t) {
        case Tok::PipePipe: return 1;
        case Tok::AmpAmp: return 2;
        case Tok::QQ: return 3;  // a ?? b: lebih lemah dari aritmetika, lebih kuat dari perbandingan
        case Tok::EqEq: case Tok::NotEq:
        case Tok::Lt: case Tok::Le: case Tok::Gt: case Tok::Ge: return 4;
        case Tok::Pipe: return 5;
        case Tok::Caret: return 6;
        case Tok::Amp: return 7;
        case Tok::Shl: case Tok::Shr: return 8;
        case Tok::Plus: case Tok::Minus: case Tok::PlusWrap: case Tok::MinusWrap:
        case Tok::PlusSat: case Tok::MinusSat: case Tok::PlusOpt: case Tok::MinusOpt: return 9;
        case Tok::Star: case Tok::Slash: case Tok::Percent:
        case Tok::StarWrap: case Tok::StarSat: case Tok::StarOpt: case Tok::SlashOpt: case Tok::PercentOpt:
            return 10;
        default: return 0;
    }
}

bool is_comparison(Tok t) { return binary_prec(t) == 4; }

class Parser {
public:
    Parser(const std::vector<Token>& toks, Diag& d) : t_(toks), diag_(d) {}

    Program run() {
        Program p;
        bool header = true;  // 'package' & 'import' hanya boleh sebelum deklarasi lain
        while (!at(Tok::Eof)) {
            if (accept(Tok::Semi)) continue;
            size_t before = pos_;
            if (at(Tok::Ident) && cur().text == "package" && peek().kind == Tok::Ident) {
                Loc l = next().loc;
                std::string nm = next().text;
                if (!header || !p.package_name.empty() || !p.imports.empty())
                    diag_.error(l, "'package' must be the first line of the file");
                else {
                    p.package_name = nm;
                    p.package_loc = l;
                }
            } else if (at(Tok::KwImport)) {
                Loc l = next().loc;
                if (!header) diag_.error(l, "imports must come before the other declarations");
                if (accept(Tok::LParen)) {
                    for (;;) {
                        while (accept(Tok::Semi)) {}
                        if (at(Tok::RParen) || at(Tok::Eof)) break;
                        size_t b2 = pos_;
                        parse_import_spec(p);
                        if (pos_ == b2) next();
                        if (!at(Tok::RParen)) expect(Tok::Semi, "after the import");
                    }
                    expect(Tok::RParen, "to close the import list");
                } else {
                    parse_import_spec(p);
                }
            } else if (at(Tok::KwType) && peek(2).kind == Tok::KwEnum) {
                p.enums.push_back(parse_enum_decl());
            } else if (at(Tok::KwType)) {
                if (auto sd = parse_type_decl()) p.structs.push_back(std::move(sd));
            } else if (at(Tok::Hash)) {
                // #max_depth(N) sebelum fungsi rekursif
                Loc hl = next().loc;
                std::string attr = expect_ident("attribute name");
                int64_t n = 0;
                if (expect(Tok::LParen)) {
                    if (at(Tok::Int)) n = (int64_t)next().int_val;
                    else error_here("expected a number");
                    expect(Tok::RParen);
                }
                while (accept(Tok::Semi)) {}
                if (attr != "max_depth") diag_.error(hl, "unknown attribute '#" + attr + "'", "known: #max_depth(N)");
                bool thr = false;
                if (!at(Tok::KwFn)) {
                    error_here("an attribute must be followed by a function");
                    continue;
                }
                auto f = parse_fn();
                f->is_thread = thr;
                f->max_depth = n < 1 ? 1 : n;
                p.fns.push_back(std::move(f));
            } else if (at(Tok::Ident) && cur().text == "unsafe" && peek().kind == Tok::Ident && peek().text == "extern") {
                next();
                next();
                if (!at(Tok::KwFn)) {
                    error_here("expected 'fn' after 'unsafe extern'");
                    sync_top();
                    continue;
                }
                p.fns.push_back(parse_extern());
            } else if (at(Tok::Ident) && cur().text == "extern" && peek().kind == Tok::KwFn) {
                error_here("foreign functions are declared 'unsafe extern fn'",
                           "the compiler cannot check C code; 'unsafe' marks every place that trusts it");
                next();
                p.fns.push_back(parse_extern());
            } else if (at(Tok::Ident) && cur().text == "thread" && peek().kind == Tok::KwFn) {
                error_here("'thread fn' was replaced by goroutines",
                           "declare a normal 'fn' and start it with 'run f(...)'; communicate through 'chan[T](n)'");
                next();
                p.fns.push_back(parse_fn());
            } else if (at(Tok::KwFn)) {
                p.fns.push_back(parse_fn());
            } else if (at(Tok::KwConst)) {
                p.consts.push_back(parse_const());
            } else if (at(Tok::KwStruct)) {
                error_here("struct declarations use 'type Name struct { ... }'");
                sync_top();
            } else {
                error_here(std::string("expected 'fn', 'type' or 'const' at top level, found ") +
                           tok_name(cur().kind));
                sync_top();
            }
            if (pos_ == before) next();  // jaminan maju
            {
                const Token& first = t_[before];
                bool hdr = first.kind == Tok::KwImport || (first.kind == Tok::Ident && first.text == "package");
                if (!hdr) header = false;
            }
            end_decl();
        }
        return p;
    }

    // "path"  atau  alias "path"
    void parse_import_spec(Program& p) {
        ImportDecl d;
        d.loc = cur().loc;
        if (at(Tok::Ident)) d.alias = next().text;
        if (!at(Tok::String)) {
            error_here(std::string("expected an import path string, found ") + found(),
                       "e.g. 'import \"myapp/utils\"'");
            return;
        }
        d.path = next().text;
        p.imports.push_back(std::move(d));
    }

private:
    const Token& cur() const { return t_[pos_]; }
    const Token& peek(size_t n = 1) const { return pos_ + n < t_.size() ? t_[pos_ + n] : t_.back(); }
    bool at(Tok k) const { return cur().kind == k; }
    const Token& next() {
        const Token& tok = t_[pos_];
        if (pos_ + 1 < t_.size()) pos_++;
        return tok;
    }
    bool accept(Tok k) {
        if (at(k)) {
            next();
            return true;
        }
        return false;
    }
    void error_here(const std::string& msg, const std::string& help = "") {
        if (pos_ == last_error_pos_) return;  // hindari banjir error di posisi yang sama
        last_error_pos_ = pos_;
        diag_.error(cur().loc, msg, help);
    }
    std::string found() const {
        if (at(Tok::Semi) && cur().auto_semi) return "end of line";
        return tok_name(cur().kind);
    }
    bool expect(Tok k, const char* what = nullptr) {
        if (accept(k)) return true;
        std::string msg = std::string("expected ") + tok_name(k);
        if (what) msg += std::string(" ") + what;
        msg += ", found " + found();
        error_here(msg);
        return false;
    }
    std::string expect_ident(const char* what) {
        if (at(Tok::Ident)) return next().text;
        error_here(std::string("expected ") + what + ", found " + found());
        return "<error>";
    }
    // ';' otomatis di dalam (...) atau {...} multi-baris diabaikan.
    void skip_auto_semis() {
        while (at(Tok::Semi) && cur().auto_semi) next();
    }

    void sync_top() {
        int depth = 0;
        while (!at(Tok::Eof)) {
            if (at(Tok::LBrace)) depth++;
            if (at(Tok::RBrace)) depth--;
            if (depth <= 0 && (at(Tok::KwFn) || at(Tok::KwType) || at(Tok::KwConst))) return;
            next();
        }
    }

    void sync_stmt() {
        int depth = 0;
        while (!at(Tok::Eof)) {
            if (at(Tok::LBrace)) depth++;
            if (at(Tok::RBrace)) {
                if (depth == 0) return;
                depth--;
            }
            if (at(Tok::Semi) && depth == 0) {
                next();
                return;
            }
            next();
        }
    }

    void end_decl() {
        if (at(Tok::Eof) || accept(Tok::Semi)) return;
        if (at(Tok::KwFn) || at(Tok::KwType) || at(Tok::KwConst)) {
            error_here("expected a new line between declarations");
            return;
        }
        error_here(std::string("unexpected ") + found() + " after declaration");
        sync_top();
    }

    bool starts_type() const { return at(Tok::Ident) || at(Tok::LBracket) || at(Tok::Amp) || at(Tok::KwFn); }

    // i64 | Point | [N]T | [..]T | []T
    TypeExpr parse_type() {
        TypeExpr te;
        te.loc = cur().loc;
        if (accept(Tok::KwFn)) {
            // fn(A, B) R
            te.kind = TypeExprKind::Fn;
            expect(Tok::LParen);
            while (!at(Tok::RParen) && !at(Tok::Eof)) {
                te.fn_params.push_back(parse_type());
                if (!accept(Tok::Comma)) break;
            }
            expect(Tok::RParen, "to close the function type");
            if (starts_type()) te.fn_ret = std::make_shared<TypeExpr>(parse_type());
            return te;
        }
        if (accept(Tok::Amp)) {
            te.kind = TypeExprKind::Ref;
            te.elem = std::make_shared<TypeExpr>(parse_type());
            return te;
        }
        if (accept(Tok::LBracket)) {
            if (accept(Tok::RBracket)) {
                te.kind = TypeExprKind::Slice;
            } else if (accept(Tok::DotDot)) {
                te.kind = TypeExprKind::Dyn;
                expect(Tok::RBracket);
            } else {
                te.kind = TypeExprKind::Array;
                if (at(Tok::Int)) te.size_val = next().int_val;
                else if (at(Tok::Ident)) te.size_name = next().text;
                else error_here("expected an array size (number or constant), '..' or ']'");
                expect(Tok::RBracket);
            }
            te.elem = std::make_shared<TypeExpr>(parse_type());
            return te;
        }
        if (at(Tok::LParen)) {
            next();
            TypeExpr inner = parse_type();
            expect(Tok::RParen);
            return optional_suffix(inner);
        }
        if (at(Tok::Ident) && cur().text == "func" && peek().kind == Tok::LParen) {
            // func(A, B) R : referensi ke fungsi bernama
            next();
            next();
            te.kind = TypeExprKind::Fn;
            te.func_ref = true;
            skip_auto_semis();
            while (!at(Tok::RParen) && !at(Tok::Eof)) {
                te.fn_params.push_back(parse_type());
                if (!accept(Tok::Comma)) break;
            }
            expect(Tok::RParen, "to close the function type");
            if (starts_type()) te.fn_ret = std::make_shared<TypeExpr>(parse_type());
            return optional_suffix(te);
        }
        te.name = expect_ident("a type name");
        if (at(Tok::Dot) && peek().kind == Tok::Ident) {
            // tipe dari package lain: utils.Point
            next();
            te.name += "." + next().text;
        }
        if (at(Tok::LBracket)) {
            // argumen tipe bawaan: Pool[T], Handle[T], Link[T]
            next();
            do {
                te.targs.push_back(parse_type());
            } while (accept(Tok::Comma));
            expect(Tok::RBracket, "to close the type arguments");
        }
        return optional_suffix(te);
    }

    // T? (bisa diulang pada elemen array: [..]i64? = array dari i64?)
    TypeExpr optional_suffix(TypeExpr te) {
        if (accept(Tok::Question)) {
            TypeExpr o;
            o.loc = te.loc;
            o.kind = TypeExprKind::Optional;
            o.elem = std::make_shared<TypeExpr>(te);
            return o;
        }
        return te;
    }

    // Field berkelompok: { a, b T; c U }  (struct dan payload varian enum)
    void parse_field_list(std::vector<FieldDecl>& out) {
        expect(Tok::LBrace);
        for (;;) {
            while (accept(Tok::Semi)) {}
            if (at(Tok::RBrace) || at(Tok::Eof)) break;
            std::vector<FieldDecl> group;
            do {
                FieldDecl f;
                f.loc = cur().loc;
                f.name = expect_ident("field name");
                group.push_back(std::move(f));
            } while (accept(Tok::Comma));
            if (at(Tok::Colon)) {
                error_here("field types are written without ':'", "write 'x i64', not 'x: i64'");
                next();
            }
            TypeExpr te = parse_type();
            for (auto& f : group) {
                f.type_expr = te;
                out.push_back(std::move(f));
            }
            if (!accept(Tok::Semi) && !accept(Tok::Comma) && !at(Tok::RBrace)) {
                error_here(std::string("expected a new line or ',' after field, found ") + found());
                sync_stmt();
            }
        }
        expect(Tok::RBrace, "to close the field list");
    }

    // type Shape enum { Circle { r f64 }  Rect { w, h f64 }  Dot }
    std::unique_ptr<EnumDecl> parse_enum_decl() {
        next();  // type
        auto ed = std::make_unique<EnumDecl>();
        ed->loc = cur().loc;
        ed->name = expect_ident("type name");
        next();  // enum
        expect(Tok::LBrace);
        for (;;) {
            while (accept(Tok::Semi) || accept(Tok::Comma)) {}
            if (at(Tok::RBrace) || at(Tok::Eof)) break;
            VariantDecl v;
            v.loc = cur().loc;
            v.name = expect_ident("variant name");
            if (at(Tok::LBrace)) {
                v.has_payload = true;
                parse_field_list(v.fields);
            }
            ed->variants.push_back(std::move(v));
            if (!accept(Tok::Semi) && !accept(Tok::Comma) && !at(Tok::RBrace)) {
                error_here(std::string("expected a new line or ',' after variant, found ") + found());
                sync_stmt();
            }
        }
        expect(Tok::RBrace, "to close enum");
        return ed;
    }

    // type Name struct { a, b T; c U }
    std::unique_ptr<StructDecl> parse_type_decl() {
        next();  // type
        auto sd = std::make_unique<StructDecl>();
        sd->loc = cur().loc;
        sd->name = expect_ident("type name");
        if (!accept(Tok::KwStruct)) {
            error_here("expected 'struct' after the type name", "only struct types exist for now: 'type " +
                                                                    sd->name + " struct { ... }'");
            sync_top();
            return nullptr;
        }
        expect(Tok::LBrace);
        for (;;) {
            while (accept(Tok::Semi)) {}
            if (at(Tok::RBrace) || at(Tok::Eof)) break;
            std::vector<FieldDecl> group;
            do {
                FieldDecl f;
                f.loc = cur().loc;
                f.name = expect_ident("field name");
                group.push_back(std::move(f));
            } while (accept(Tok::Comma));
            if (at(Tok::Colon)) {
                error_here("field types are written without ':'", "write 'x i64', not 'x: i64'");
                next();
            }
            TypeExpr te = parse_type();
            for (auto& f : group) {
                f.type_expr = te;
                sd->fields.push_back(std::move(f));
            }
            if (!accept(Tok::Semi) && !accept(Tok::Comma) && !at(Tok::RBrace)) {
                error_here(std::string("expected a new line or ',' after field, found ") + found());
                sync_stmt();
            }
        }
        expect(Tok::RBrace, "to close struct");
        return sd;
    }

    std::unique_ptr<ConstDecl> parse_const() {
        auto c = std::make_unique<ConstDecl>();
        next();  // const
        c->loc = cur().loc;
        c->name = expect_ident("constant name");
        if (starts_type()) {
            c->has_type = true;
            c->type = parse_type();
        }
        expect(Tok::Eq);
        c->value = parse_expr();
        return c;
    }

    // Parameter gaya Go: (a, b i32, c bool). Nama tanpa tipe memakai tipe berikutnya.
    std::vector<Param> parse_params() {
        std::vector<Param> params;
        std::vector<bool> typed;
        expect(Tok::LParen);
        skip_auto_semis();
        while (!at(Tok::RParen) && !at(Tok::Eof)) {
            Param p;
            p.loc = cur().loc;
            p.name = expect_ident("parameter name");
            if (at(Tok::Colon)) {
                error_here("parameter types are written without ':'", "write 'a i64', not 'a: i64'");
                next();
            }
            bool has_type = starts_type();
            if (has_type) p.type = parse_type();
            params.push_back(std::move(p));
            typed.push_back(has_type);
            skip_auto_semis();
            if (!accept(Tok::Comma)) break;
            skip_auto_semis();
        }
        expect(Tok::RParen, "to close parameter list");
        // isi tipe dari belakang: (a, b i32) → a i32, b i32
        bool have = false;
        TypeExpr last;
        for (size_t i = params.size(); i-- > 0;) {
            if (typed[i]) {
                last = params[i].type;
                have = true;
            } else if (have) {
                params[i].type = last;
            } else {
                diag_.error(params[i].loc, "missing type for parameter '" + params[i].name + "'",
                            "write it like 'a i64' or 'a, b i64'");
                params[i].type.loc = params[i].loc;
                params[i].type.name = "<error>";
            }
        }
        return params;
    }

    // unsafe extern fn name(params) R   — tanpa badan; simbol C dengan nama yang sama
    std::unique_ptr<FnDecl> parse_extern() {
        auto fn = std::make_unique<FnDecl>();
        next();  // fn
        fn->is_extern = true;
        fn->loc = cur().loc;
        fn->name = expect_ident("function name");
        fn->params = parse_params();
        if (starts_type()) fn->ret_types.push_back(parse_type());
        fn->body = std::make_unique<Block>();
        if (at(Tok::LBrace)) error_here("an extern function has no body");
        return fn;
    }

    std::unique_ptr<FnDecl> parse_fn() {
        auto fn = std::make_unique<FnDecl>();
        next();  // fn
        if (at(Tok::LParen)) {
            // receiver: fn (p Point) name(...)
            next();
            fn->has_receiver = true;
            fn->receiver.loc = cur().loc;
            fn->receiver.name = expect_ident("receiver name");
            fn->receiver.type = parse_type();
            expect(Tok::RParen, "to close the receiver");
        }
        fn->loc = cur().loc;
        fn->name = expect_ident("function name");
        if (accept(Tok::LBracket)) {
            // fn max[T](a, b T) T
            do {
                fn->type_params.push_back(expect_ident("type parameter name"));
            } while (accept(Tok::Comma));
            expect(Tok::RBracket, "to close the type parameters");
        }
        fn->params = parse_params();
        if (at(Tok::Arrow)) {
            error_here("return types are written without '->'", "write 'fn f() i64' or 'fn f() (i64, bool)'");
            next();
        }
        if (starts_type()) {
            fn->ret_types.push_back(parse_type());
        } else if (at(Tok::LParen)) {
            next();
            do {
                fn->ret_types.push_back(parse_type());
            } while (accept(Tok::Comma));
            expect(Tok::RParen, "to close the result list");
        }
        if (!at(Tok::LBrace)) {
            error_here(std::string("expected '{' to start the function body, found ") + found(),
                       at(Tok::Semi) ? "'{' must be on the same line as 'fn'" : "");
            sync_top();
            fn->body = std::make_unique<Block>();
            return fn;
        }
        fn->body = parse_block();
        return fn;
    }

    BlockPtr parse_block() {
        auto b = std::make_unique<Block>();
        b->loc = cur().loc;
        expect(Tok::LBrace);
        for (;;) {
            while (accept(Tok::Semi)) {}
            if (at(Tok::RBrace) || at(Tok::Eof)) break;
            size_t before = pos_;
            StmtPtr s = parse_stmt();
            if (s) b->stmts.push_back(std::move(s));
            if (pos_ == before) next();
            end_stmt();
        }
        b->end_loc = cur().loc;
        expect(Tok::RBrace, "to close block");
        return b;
    }

    void end_stmt() {
        if (accept(Tok::Semi) || at(Tok::RBrace) || at(Tok::Eof)) return;
        if (pos_ > 0 && t_[pos_ - 1].kind == Tok::Semi) return;  // pemulihan error sudah melewati akhir baris
        error_here(std::string("expected a new line or ';' after statement, found ") + found());
        sync_stmt();
    }

    bool starts_short_decl() const {
        // a := ...   atau   a, b := ...
        size_t i = pos_;
        for (;;) {
            if (t_[i].kind != Tok::Ident) return false;
            i++;
            if (i >= t_.size()) return false;
            if (t_[i].kind == Tok::ColonEq) return true;
            if (t_[i].kind != Tok::Comma) return false;
            i++;
            if (i >= t_.size()) return false;
        }
    }

    StmtPtr parse_stmt() {
        Loc loc = cur().loc;
        switch (cur().kind) {
            case Tok::KwVar: return parse_var();
            case Tok::KwIf: return parse_if();
            case Tok::KwMatch: return parse_match();
            case Tok::KwFor: return parse_for();
            case Tok::KwBreak:
            case Tok::KwContinue: {
                auto s = std::make_unique<Stmt>(at(Tok::KwBreak) ? StmtKind::Break : StmtKind::Continue, loc);
                next();
                return s;
            }
            case Tok::KwReturn: {
                next();
                auto s = std::make_unique<Stmt>(StmtKind::Return, loc);
                auto at_from = [&]() { return at(Tok::Ident) && cur().text == "from" && peek().kind == Tok::Ident; };
                if (!at(Tok::Semi) && !at(Tok::RBrace) && !at_from()) {
                    s->exprs.push_back(parse_expr());
                    while (accept(Tok::Comma)) s->exprs.push_back(parse_expr());
                }
                if (at_from()) {
                    // return e from f
                    next();
                    s->from_loc = cur().loc;
                    s->from_name = next().text;
                }
                return s;
            }
            case Tok::LBrace: {
                auto s = std::make_unique<Stmt>(StmtKind::Block, loc);
                s->body = parse_block();
                return s;
            }
            case Tok::KwFn:
            case Tok::KwType:
                error_here("nested declarations are not supported", "move it to the top level");
                sync_stmt();
                return nullptr;
            case Tok::KwConst:
                error_here("constants are declared at the top level");
                sync_stmt();
                return nullptr;
            case Tok::Ident: {
                const std::string& w = cur().text;
                if (w == "select" && peek().kind == Tok::LBrace) return parse_select();
                if (w == "defer" && (peek().kind == Tok::LBrace || peek().kind == Tok::Ident)) {
                    // defer f(args)  /  defer x.m(args)  /  defer { ... }
                    next();
                    auto s = std::make_unique<Stmt>(StmtKind::Defer, loc);
                    if (at(Tok::LBrace)) {
                        s->body = parse_block();
                    } else {
                        s->e1 = parse_expr(true);
                        if (s->e1->kind != ExprKind::Call)
                            diag_.error(s->e1->loc, "'defer' takes a function call or a block",
                                        "e.g. 'defer close_file(f)' or 'defer { ... }'");
                    }
                    return s;
                }
                if (w == "let" || w == "while" || w == "loop" || w == "guard") {
                    const char* help = w == "let"     ? "declare with 'x := value' or 'var x T'"
                                       : w == "while" ? "use 'for condition { ... }'"
                                       : w == "loop"  ? "use 'for { ... }'"
                                                      : "use 'if !condition { return ... }'";
                    error_here("'" + w + "' is not a tree-lang keyword", help);
                    sync_stmt();
                    return nullptr;
                }
                if (starts_short_decl()) return parse_short_decl();
                if (w == "_" && peek().kind == Tok::Eq) {
                    next();
                    next();
                    auto s = std::make_unique<Stmt>(StmtKind::Discard, loc);
                    s->e1 = parse_expr();
                    return s;
                }
                break;
            }
            default: break;
        }
        return parse_simple_stmt();
    }

    // expr | expr = e | expr op= e | expr++ | expr--
    StmtPtr parse_simple_stmt() {
        Loc loc = cur().loc;
        ExprPtr e = parse_expr();
        Tok k = cur().kind;
        if (k == Tok::Eq || k == Tok::PlusEq || k == Tok::MinusEq || k == Tok::StarEq || k == Tok::SlashEq ||
            k == Tok::PercentEq) {
            next();
            auto s = std::make_unique<Stmt>(StmtKind::Assign, loc);
            s->op = k;
            s->e1 = std::move(e);
            s->e2 = parse_expr();
            return s;
        }
        if (k == Tok::PlusPlus || k == Tok::MinusMinus) {
            Loc oploc = next().loc;
            auto s = std::make_unique<Stmt>(StmtKind::Assign, loc);
            s->op = k == Tok::PlusPlus ? Tok::PlusEq : Tok::MinusEq;
            s->e1 = std::move(e);
            s->e2 = std::make_unique<Expr>(ExprKind::IntLit, oploc);
            s->e2->int_val = 1;
            return s;
        }
        if (k == Tok::ColonEq) {
            error_here("':=' declares new variables and needs plain names on the left");
            sync_stmt();
            return nullptr;
        }
        auto s = std::make_unique<Stmt>(StmtKind::ExprStmt, loc);
        s->e1 = std::move(e);
        return s;
    }

    // a := e   /   a, b := f()
    StmtPtr parse_short_decl() {
        auto s = std::make_unique<Stmt>(StmtKind::Let, cur().loc);
        s->is_var = false;
        do {
            s->name_locs.push_back(cur().loc);
            s->names.push_back(next().text);
        } while (accept(Tok::Comma));
        expect(Tok::ColonEq);
        s->e1 = parse_expr();
        return s;
    }

    // var a T  /  var a T = e  /  var a = e  /  var a, b = f()
    StmtPtr parse_var() {
        auto s = std::make_unique<Stmt>(StmtKind::Let, next().loc);  // var
        s->is_var = true;
        do {
            s->name_locs.push_back(cur().loc);
            s->names.push_back(expect_ident("variable name"));
        } while (accept(Tok::Comma));
        if (at(Tok::Colon)) {
            error_here("variable types are written without ':'", "write 'var x i64', not 'var x: i64'");
            next();
        }
        if (starts_type()) {
            s->has_type = true;
            s->type = parse_type();
        }
        if (accept(Tok::Eq)) {
            s->e1 = parse_expr();
        } else if (s->has_type) {
            s->zero_init = true;
        } else {
            error_here("'var' needs a type or a value", "write 'var x i64' or 'var x = 0'");
        }
        return s;
    }

    // select {
    //     x := <-ch { }      <-ch { }      ok := ch <- v { }      ch <- v { }
    //     timeout(ms) { }    default { }
    // }
    StmtPtr parse_select() {
        Loc loc = next().loc;  // select
        auto s = std::make_unique<Stmt>(StmtKind::Select, loc);
        expect(Tok::LBrace);
        for (;;) {
            while (accept(Tok::Semi)) {}
            if (at(Tok::RBrace) || at(Tok::Eof)) break;
            size_t before = pos_;
            SelectArm arm;
            arm.loc = cur().loc;
            if (at(Tok::Ident) && cur().text == "default" && peek().kind == Tok::LBrace) {
                next();
                arm.kind = SelectArm::Default;
            } else if (at(Tok::Ident) && cur().text == "timeout" && peek().kind == Tok::LParen) {
                next();
                arm.kind = SelectArm::Timeout;
                auto args = parse_args();
                if (args.size() != 1) error_here("timeout takes the number of milliseconds");
                else arm.op = std::move(args[0]);
            } else {
                if (at(Tok::Ident) && (peek().kind == Tok::ColonEq || (cur().text == "_" && peek().kind == Tok::Eq))) {
                    arm.bind_loc = cur().loc;
                    arm.bind = next().text;
                    next();  // := atau =
                }
                arm.op = parse_expr(true);
                if (arm.op->kind == ExprKind::ChanRecv) arm.kind = SelectArm::Recv;
                else if (arm.op->kind == ExprKind::ChanSend) arm.kind = SelectArm::Send;
                else error_here("a select arm is '<-ch', 'x := <-ch', 'ch <- v', 'ok := ch <- v', 'timeout(ms)' or 'default'");
            }
            if (!at(Tok::LBrace)) {
                error_here(std::string("expected '{' to start the select arm, found ") + found());
                sync_stmt();
                if (pos_ == before) next();
                continue;
            }
            arm.body = parse_block();
            s->sel.push_back(std::move(arm));
        }
        expect(Tok::RBrace, "to close select");
        return s;
    }

    // match e { Variant(x) { } Variant(&r) { } some(x) { } none { } 1, 2 { } _ { } }
    StmtPtr parse_match() {
        Loc loc = next().loc;  // match
        auto s = std::make_unique<Stmt>(StmtKind::Match, loc);
        s->e1 = parse_expr(true);
        expect(Tok::LBrace, "to start the match arms");
        for (;;) {
            while (accept(Tok::Semi)) {}
            if (at(Tok::RBrace) || at(Tok::Eof)) break;
            size_t before = pos_;
            MatchArm arm;
            arm.loc = cur().loc;
            auto binding = [&]() {
                if (!accept(Tok::LParen)) return;
                arm.bind_ref = accept(Tok::Amp);
                arm.bind_loc = cur().loc;
                arm.bind = expect_ident("binding name");
                expect(Tok::RParen);
            };
            if (at(Tok::Ident) && cur().text == "_") {
                next();
                arm.kind = MatchArm::Wild;
            } else if (accept(Tok::KwNone)) {
                arm.kind = MatchArm::None;
            } else if (accept(Tok::KwSome)) {
                arm.kind = MatchArm::Some;
                binding();
            } else if (at(Tok::Ident)) {
                arm.kind = MatchArm::Variant;
                arm.name = next().text;
                binding();
            } else {
                arm.kind = MatchArm::Values;
                do {
                    arm.values.push_back(parse_unary());
                } while (accept(Tok::Comma));
            }
            if (!at(Tok::LBrace)) {
                error_here(std::string("expected '{' to start the match arm, found ") + found());
                sync_stmt();
                if (pos_ == before) next();
                continue;
            }
            arm.body = parse_block();
            s->arms.push_back(std::move(arm));
        }
        expect(Tok::RBrace, "to close match");
        return s;
    }

    StmtPtr parse_if() {
        Loc loc = next().loc;  // if
        auto s = std::make_unique<Stmt>(StmtKind::If, loc);
        if (accept(Tok::KwSome)) {
            // if some x := opt { ... }
            s->if_some = true;
            s->name_locs.push_back(cur().loc);
            s->names.push_back(expect_ident("binding name"));
            expect(Tok::ColonEq, "in 'if some x := value'");
        }
        s->e1 = parse_expr(true);
        s->body = parse_block();
        if (at(Tok::Semi) && cur().auto_semi && peek().kind == Tok::KwElse) {
            error_here("'else' must be on the same line as the closing '}'", "write '} else {'");
            next();
        }
        if (accept(Tok::KwElse)) {
            if (at(Tok::KwIf)) {
                auto b = std::make_unique<Block>();
                b->loc = cur().loc;
                b->stmts.push_back(parse_if());
                b->end_loc = b->stmts.back()->loc;
                s->else_body = std::move(b);
            } else {
                s->else_body = parse_block();
            }
        }
        return s;
    }

    // for { }  /  for cond { }  /  for i := range [a..]b { }
    StmtPtr parse_for() {
        Loc loc = next().loc;  // for
        if (at(Tok::LBrace)) {
            auto s = std::make_unique<Stmt>(StmtKind::Loop, loc);
            s->body = parse_block();
            return s;
        }
        bool two = at(Tok::Ident) && peek().kind == Tok::Comma && peek(2).kind == Tok::Ident &&
                   peek(3).kind == Tok::ColonEq && peek(4).kind == Tok::KwRange;
        if (two || (at(Tok::Ident) && peek().kind == Tok::ColonEq && peek(2).kind == Tok::KwRange)) {
            auto s = std::make_unique<Stmt>(StmtKind::For, loc);
            s->name_locs.push_back(cur().loc);
            s->names.push_back(next().text);
            if (two) {
                next();  // ,
                s->name_locs.push_back(cur().loc);
                s->names.push_back(next().text);
            }
            next();  // :=
            next();  // range
            ExprPtr first = parse_expr(true);
            if (accept(Tok::DotDot)) {
                s->e1 = std::move(first);
                s->e2 = parse_expr(true);
            } else {
                s->e2 = std::move(first);
            }
            s->body = parse_block();
            return s;
        }
        if (at(Tok::Ident) && peek().kind == Tok::Ident && peek().text == "in") {
            error_here("range loops are written 'for i := range n { ... }'");
            sync_stmt();
            return nullptr;
        }
        auto s = std::make_unique<Stmt>(StmtKind::While, loc);
        s->e1 = parse_expr(true);
        s->body = parse_block();
        return s;
    }

    // ---------------- ekspresi ----------------

    ExprPtr parse_expr(bool no_struct = false) {
        bool saved = no_struct_;
        no_struct_ = no_struct;
        ExprPtr e = parse_binary(1);
        if (at(Tok::LArrow)) {
            // ch <- v  →  bool (false bila channel tertutup)
            auto sd = std::make_unique<Expr>(ExprKind::ChanSend, next().loc);
            sd->lhs = std::move(e);
            sd->rhs = parse_binary(1);
            e = std::move(sd);
        }
        no_struct_ = saved;
        return e;
    }

    ExprPtr parse_binary(int min_prec) {
        ExprPtr lhs = parse_cast();
        for (;;) {
            Tok op = cur().kind;
            int prec = binary_prec(op);
            if (prec == 0 || prec < min_prec) break;
            Loc loc = next().loc;
            ExprPtr rhs = parse_binary(prec + 1);
            auto e = std::make_unique<Expr>(ExprKind::Binary, loc);
            e->op = op;
            e->lhs = std::move(lhs);
            e->rhs = std::move(rhs);
            lhs = std::move(e);
            if (is_comparison(op) && is_comparison(cur().kind)) {
                error_here("comparison operators cannot be chained", "use '&&', e.g. 'a < b && b < c'");
            }
        }
        return lhs;
    }

    // 'as' mengikat lebih lemah dari unary: '-x as u8' = '(-x) as u8'
    ExprPtr parse_cast() {
        ExprPtr e = parse_unary();
        while (at(Tok::KwAs) || at(Tok::KwAsBang) || at(Tok::KwAsWrap) || at(Tok::KwAsSat) || at(Tok::KwAsOpt)) {
            Loc loc = cur().loc;
            Tok k = next().kind;
            auto c = std::make_unique<Expr>(ExprKind::Cast, loc);
            c->cast_kind = k == Tok::KwAs       ? CastKind::Plain
                           : k == Tok::KwAsBang ? CastKind::Checked
                           : k == Tok::KwAsWrap ? CastKind::Wrap
                           : k == Tok::KwAsOpt  ? CastKind::Opt
                                                : CastKind::Sat;
            c->lhs = std::move(e);
            c->cast_type = parse_type();
            e = std::move(c);
        }
        return e;
    }

    ExprPtr parse_unary() {
        if (at(Tok::LArrow)) {
            // <-ch  →  T?
            auto r = std::make_unique<Expr>(ExprKind::ChanRecv, next().loc);
            r->lhs = parse_unary();
            return r;
        }
        if (at(Tok::Minus) || at(Tok::Bang) || at(Tok::Tilde) || at(Tok::Amp)) {
            Loc loc = cur().loc;
            Tok op = next().kind;
            ExprPtr operand = parse_unary();
            // '-' + literal angka → literal negatif (agar -128 muat di i8)
            if (op == Tok::Minus && operand->kind == ExprKind::IntLit && !operand->int_neg) {
                operand->int_neg = operand->int_val != 0;
                operand->loc = loc;
                return operand;
            }
            if (op == Tok::Minus && operand->kind == ExprKind::FloatLit) {
                operand->float_val = -operand->float_val;
                operand->loc = loc;
                return operand;
            }
            auto e = std::make_unique<Expr>(ExprKind::Unary, loc);
            e->op = op;
            e->lhs = std::move(operand);
            return e;
        }
        return parse_postfix();
    }

    std::vector<ExprPtr> parse_args() {
        std::vector<ExprPtr> args;
        expect(Tok::LParen);
        bool saved = no_struct_;
        no_struct_ = false;
        skip_auto_semis();
        while (!at(Tok::RParen) && !at(Tok::Eof)) {
            args.push_back(parse_expr());
            skip_auto_semis();
            if (!accept(Tok::Comma)) break;
            skip_auto_semis();
        }
        no_struct_ = saved;
        expect(Tok::RParen, "to close argument list");
        return args;
    }

    ExprPtr parse_postfix() {
        ExprPtr e = parse_primary();
        while (at(Tok::Dot) || at(Tok::LBracket)) {
            if (at(Tok::LBracket)) {
                Loc loc = next().loc;
                auto ix = std::make_unique<Expr>(ExprKind::Index, loc);
                bool saved = no_struct_;
                no_struct_ = false;
                ix->lhs = std::move(e);
                // a[i]  /  a[i..j]  /  a[i..]  /  a[..j]
                if (!at(Tok::DotDot)) ix->rhs = parse_binary(1);
                if (accept(Tok::DotDot)) {
                    ix->kind = ExprKind::SliceOf;
                    if (!at(Tok::RBracket)) ix->end = parse_binary(1);
                }
                no_struct_ = saved;
                expect(Tok::RBracket, "to close the index");
                e = std::move(ix);
                continue;
            }
            next();
            Loc loc = cur().loc;
            std::string name = expect_ident("field or method name");
            if (at(Tok::LParen)) {
                auto c = std::make_unique<Expr>(ExprKind::Call, loc);
                c->str = name;
                c->method_call = true;
                c->args.push_back(std::move(e));
                for (auto& a : parse_args()) c->args.push_back(std::move(a));
                if (at(Tok::LBrace) && !no_struct_) c->args.push_back(parse_block_lit());
                e = std::move(c);
            } else if (at(Tok::LBrace) && !no_struct_ && e->kind == ExprKind::Name) {
                // literal varian enum: Shape.Circle{r: 1.0}   /   struct dari package: utils.Point{...}
                e = parse_struct_lit(e->str + "." + name, loc);
            } else if (at(Tok::LBrace) && !no_struct_ && e->kind == ExprKind::Field && e->lhs->kind == ExprKind::Name) {
                // varian enum dari package lain: utils.Shape.Circle{r: 1.0}
                e = parse_struct_lit(e->lhs->str + "." + e->str + "." + name, loc);
            } else {
                auto f = std::make_unique<Expr>(ExprKind::Field, loc);
                f->str = name;
                f->lhs = std::move(e);
                e = std::move(f);
            }
        }
        return e;
    }

    ExprPtr parse_primary() {
        Loc loc = cur().loc;
        switch (cur().kind) {
            case Tok::Int: {
                auto e = std::make_unique<Expr>(ExprKind::IntLit, loc);
                e->int_val = next().int_val;
                return e;
            }
            case Tok::Float: {
                auto e = std::make_unique<Expr>(ExprKind::FloatLit, loc);
                e->float_val = next().float_val;
                return e;
            }
            case Tok::String: {
                auto e = std::make_unique<Expr>(ExprKind::StrLit, loc);
                e->str = next().text;
                return e;
            }
            case Tok::KwNone:
                next();
                return std::make_unique<Expr>(ExprKind::NoneLit, loc);
            case Tok::KwTrue:
            case Tok::KwFalse: {
                auto e = std::make_unique<Expr>(ExprKind::BoolLit, loc);
                e->bool_val = next().kind == Tok::KwTrue;
                return e;
            }
            case Tok::LParen: {
                next();
                bool saved = no_struct_;
                no_struct_ = false;
                skip_auto_semis();
                ExprPtr e = parse_binary(1);
                skip_auto_semis();
                no_struct_ = saved;
                expect(Tok::RParen);
                return e;
            }
            case Tok::LBracket: {
                // [3]i64{1, 2, 3}   /   [..]i64{1, 2}
                auto e = std::make_unique<Expr>(ExprKind::ArrayLit, loc);
                e->array_type = parse_type();
                if (!at(Tok::LBrace)) {
                    error_here("expected '{' with the elements of the array literal", "e.g. '[3]i64{1, 2, 3}'");
                    return e;
                }
                next();
                bool saved = no_struct_;
                no_struct_ = false;
                skip_auto_semis();
                while (!at(Tok::RBrace) && !at(Tok::Eof)) {
                    e->args.push_back(parse_expr());
                    skip_auto_semis();
                    if (!accept(Tok::Comma)) break;
                    skip_auto_semis();
                }
                no_struct_ = saved;
                expect(Tok::RBrace, "to close the array literal");
                return e;
            }
            case Tok::Ident: {
                std::string name = next().text;
                if (name == "run" && at(Tok::Ident) &&
                    (peek().kind == Tok::LParen ||
                     (peek().kind == Tok::Dot && peek(2).kind == Tok::Ident && peek(3).kind == Tok::LParen))) {
                    // run worker(args) / run pkg.Worker(args): goroutine baru
                    auto sp = std::make_unique<Expr>(ExprKind::Spawn, loc);
                    sp->str = next().text;
                    if (accept(Tok::Dot)) sp->str += "." + next().text;
                    sp->args = parse_args();
                    return sp;
                }
                if (name == "spawn" && at(Tok::Ident) && peek().kind == Tok::LParen) {
                    error_here("'spawn' was replaced by goroutines: write 'run " + cur().text + "(...)'");
                }
                if (name == "chan" && at(Tok::LBracket)) {
                    // chan[T](kapasitas)
                    next();
                    auto c = std::make_unique<Expr>(ExprKind::ChanMake, loc);
                    c->array_type = parse_type();
                    expect(Tok::RBracket, "to close the channel element type");
                    if (!at(Tok::LParen)) {
                        error_here("a channel needs a capacity: 'chan[T](16)' (0 = unbuffered)");
                        return c;
                    }
                    c->args = parse_args();
                    return c;
                }
                if (at(Tok::LParen)) {
                    auto c = std::make_unique<Expr>(ExprKind::Call, loc);
                    c->str = name;
                    c->args = parse_args();
                    if (at(Tok::LBrace) && !no_struct_) c->args.push_back(parse_block_lit());
                    return c;
                }
                if (at(Tok::LBrace) && !no_struct_) return parse_struct_lit(name, loc);
                auto e = std::make_unique<Expr>(ExprKind::Name, loc);
                e->str = name;
                return e;
            }
            default: break;
        }
        error_here(std::string("expected an expression, found ") + found());
        auto e = std::make_unique<Expr>(ExprKind::IntLit, loc);
        e->type = Types::error();
        return e;
    }

    // Trailing block: { a, b => statement... }  /  { ekspresi dengan 'it' }
    ExprPtr parse_block_lit() {
        auto e = std::make_unique<Expr>(ExprKind::BlockLit, cur().loc);
        auto b = std::make_shared<Block>();
        b->loc = next().loc;  // {
        skip_auto_semis();
        // parameter: Ident (, Ident)* =>
        size_t i = pos_;
        bool has_params = false;
        while (i + 1 < t_.size() && t_[i].kind == Tok::Ident) {
            i++;
            if (t_[i].kind == Tok::FatArrow) {
                has_params = true;
                break;
            }
            if (t_[i].kind != Tok::Comma) break;
            i++;
        }
        if (has_params) {
            do {
                e->block_param_locs.push_back(cur().loc);
                e->block_params.push_back(next().text);
            } while (accept(Tok::Comma));
            expect(Tok::FatArrow);
        }
        bool saved = no_struct_;
        no_struct_ = false;
        for (;;) {
            while (accept(Tok::Semi)) {}
            if (at(Tok::RBrace) || at(Tok::Eof)) break;
            size_t before = pos_;
            StmtPtr s = parse_stmt();
            if (s) b->stmts.push_back(std::move(s));
            if (pos_ == before) next();
            end_stmt();
        }
        no_struct_ = saved;
        b->end_loc = cur().loc;
        expect(Tok::RBrace, "to close the block");
        e->block = b;
        return e;
    }

    // Point{x: 1, y: 2}  /  Point{1, 2}  (boleh multi-baris, koma akhir opsional)
    ExprPtr parse_struct_lit(const std::string& name, Loc loc) {
        auto e = std::make_unique<Expr>(ExprKind::StructLit, loc);
        e->str = name;
        next();  // {
        skip_auto_semis();
        bool named = at(Tok::Ident) && peek().kind == Tok::Colon;
        while (!at(Tok::RBrace) && !at(Tok::Eof)) {
            e->field_locs.push_back(cur().loc);
            if (named) {
                e->field_names.push_back(expect_ident("field name"));
                expect(Tok::Colon);
            }
            e->args.push_back(parse_expr());
            skip_auto_semis();
            if (!accept(Tok::Comma)) break;
            skip_auto_semis();
        }
        expect(Tok::RBrace, "to close struct literal");
        return e;
    }

    const std::vector<Token>& t_;
    Diag& diag_;
    size_t pos_ = 0;
    size_t last_error_pos_ = (size_t)-1;
    bool no_struct_ = false;
};

}  // namespace

Program parse(const std::vector<Token>& toks, Diag& diag) {
    Parser p(toks, diag);
    return p.run();
}

}  // namespace tl
