#include "lexer.hpp"

#include <cstdlib>
#include <unordered_map>

namespace tl {

const char* tok_name(Tok t) {
    switch (t) {
        case Tok::Eof: return "end of file";
        case Tok::Ident: return "identifier";
        case Tok::Int: return "integer literal";
        case Tok::Float: return "float literal";
        case Tok::String: return "string literal";
        case Tok::KwFn: return "'fn'";
        case Tok::KwVar: return "'var'";
        case Tok::KwConst: return "'const'";
        case Tok::KwStruct: return "'struct'";
        case Tok::KwType: return "'type'";
        case Tok::KwIf: return "'if'";
        case Tok::KwElse: return "'else'";
        case Tok::KwFor: return "'for'";
        case Tok::KwRange: return "'range'";
        case Tok::KwBreak: return "'break'";
        case Tok::KwContinue: return "'continue'";
        case Tok::KwReturn: return "'return'";
        case Tok::KwImport: return "'import'";
        case Tok::KwTrue: return "'true'";
        case Tok::KwFalse: return "'false'";
        case Tok::KwEnum: return "'enum'";
        case Tok::KwMatch: return "'match'";
        case Tok::KwNone: return "'none'";
        case Tok::KwSome: return "'some'";
        case Tok::KwAsOpt: return "'as?'";
        case Tok::PlusOpt: return "'+?'";
        case Tok::MinusOpt: return "'-?'";
        case Tok::StarOpt: return "'*?'";
        case Tok::SlashOpt: return "'/?'";
        case Tok::PercentOpt: return "'%?'";
        case Tok::Question: return "'?'";
        case Tok::QQ: return "'??'";
        case Tok::FatArrow: return "'=>'";
        case Tok::LArrow: return "'<-'";
        case Tok::KwAs: return "'as'";
        case Tok::KwAsBang: return "'as!'";
        case Tok::KwAsWrap: return "'as%'";
        case Tok::KwAsSat: return "'as|'";
        case Tok::LParen: return "'('";
        case Tok::RParen: return "')'";
        case Tok::LBrace: return "'{'";
        case Tok::RBrace: return "'}'";
        case Tok::LBracket: return "'['";
        case Tok::RBracket: return "']'";
        case Tok::Comma: return "','";
        case Tok::Semi: return "';'";
        case Tok::Colon: return "':'";
        case Tok::ColonEq: return "':='";
        case Tok::Dot: return "'.'";
        case Tok::DotDot: return "'..'";
        case Tok::Arrow: return "'->'";
        case Tok::Hash: return "'#'";
        case Tok::Plus: return "'+'";
        case Tok::Minus: return "'-'";
        case Tok::Star: return "'*'";
        case Tok::Slash: return "'/'";
        case Tok::Percent: return "'%'";
        case Tok::PlusWrap: return "'+%'";
        case Tok::MinusWrap: return "'-%'";
        case Tok::StarWrap: return "'*%'";
        case Tok::PlusSat: return "'+|'";
        case Tok::MinusSat: return "'-|'";
        case Tok::StarSat: return "'*|'";
        case Tok::Amp: return "'&'";
        case Tok::Pipe: return "'|'";
        case Tok::Caret: return "'^'";
        case Tok::Tilde: return "'~'";
        case Tok::Shl: return "'<<'";
        case Tok::Shr: return "'>>'";
        case Tok::AmpAmp: return "'&&'";
        case Tok::PipePipe: return "'||'";
        case Tok::Bang: return "'!'";
        case Tok::Eq: return "'='";
        case Tok::EqEq: return "'=='";
        case Tok::NotEq: return "'!='";
        case Tok::Lt: return "'<'";
        case Tok::Le: return "'<='";
        case Tok::Gt: return "'>'";
        case Tok::Ge: return "'>='";
        case Tok::PlusEq: return "'+='";
        case Tok::MinusEq: return "'-='";
        case Tok::StarEq: return "'*='";
        case Tok::SlashEq: return "'/='";
        case Tok::PercentEq: return "'%='";
        case Tok::PlusPlus: return "'++'";
        case Tok::MinusMinus: return "'--'";
    }
    return "?";
}

namespace {

const std::unordered_map<std::string, Tok> kKeywords = {
    {"fn", Tok::KwFn},         {"var", Tok::KwVar},           {"const", Tok::KwConst},
    {"struct", Tok::KwStruct}, {"type", Tok::KwType},         {"if", Tok::KwIf},
    {"else", Tok::KwElse},     {"for", Tok::KwFor},           {"range", Tok::KwRange},
    {"break", Tok::KwBreak},   {"continue", Tok::KwContinue}, {"return", Tok::KwReturn},
    {"import", Tok::KwImport}, {"true", Tok::KwTrue},         {"false", Tok::KwFalse},
    {"enum", Tok::KwEnum},     {"match", Tok::KwMatch},       {"none", Tok::KwNone},
    {"some", Tok::KwSome},
    {"as", Tok::KwAs},
};

// Aturan Go: ';' disisipkan di akhir baris bila token terakhir bisa mengakhiri statement.
bool ends_statement(Tok t) {
    switch (t) {
        case Tok::Ident: case Tok::Int: case Tok::Float: case Tok::String:
        case Tok::KwBreak: case Tok::KwContinue: case Tok::KwReturn:
        case Tok::KwTrue: case Tok::KwFalse: case Tok::KwNone:
        case Tok::RParen: case Tok::RBracket: case Tok::RBrace: case Tok::Question:
        case Tok::PlusPlus: case Tok::MinusMinus:
            return true;
        default: return false;
    }
}

class Lexer {
public:
    Lexer(const SourceFile& f, Diag& d) : src_(f.text), file_(&f), diag_(d) {}

    std::vector<Token> run() {
        std::vector<Token> out;
        // lewati BOM UTF-8 (umum pada file dari editor Windows)
        if (src_.size() >= 3 && (unsigned char)src_[0] == 0xEF && (unsigned char)src_[1] == 0xBB &&
            (unsigned char)src_[2] == 0xBF)
            pos_ = 3;
        for (;;) {
            Loc nl_loc{line_, col_, file_};
            bool newline = skip_space(nl_loc);
            if ((newline || pos_ >= src_.size()) && !out.empty() && ends_statement(out.back().kind)) {
                Token semi;
                semi.kind = Tok::Semi;
                semi.loc = newline ? nl_loc : Loc{line_, col_, file_};
                semi.auto_semi = true;
                out.push_back(semi);
            }
            Token t;
            t.loc = Loc{line_, col_, file_};
            if (pos_ >= src_.size()) {
                t.kind = Tok::Eof;
                out.push_back(t);
                return out;
            }
            char c = src_[pos_];
            if (is_ident_start(c)) {
                lex_ident(t);
            } else if (is_digit(c)) {
                lex_number(t);
            } else if (c == '"') {
                lex_string(t);
            } else {
                lex_punct(t);
            }
            out.push_back(std::move(t));
        }
    }

private:
    static bool is_digit(char c) { return c >= '0' && c <= '9'; }
    static bool is_ident_start(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }
    static bool is_ident(char c) { return is_ident_start(c) || is_digit(c); }

    char peek(size_t off = 0) const { return pos_ + off < src_.size() ? src_[pos_ + off] : '\0'; }

    void advance() {
        if (src_[pos_] == '\n') {
            line_++;
            col_ = 1;
        } else {
            col_++;
        }
        pos_++;
    }

    // Lewati spasi dan komentar. true jika melewati baris baru (nl_loc = posisinya).
    bool skip_space(Loc& nl_loc) {
        bool newline = false;
        auto mark = [&]() {
            if (!newline) nl_loc = Loc{line_, col_, file_};
            newline = true;
        };
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == '\n') {
                mark();
                advance();
            } else if (c == ' ' || c == '\t' || c == '\r') {
                advance();
            } else if (c == '/' && peek(1) == '/') {
                while (pos_ < src_.size() && src_[pos_] != '\n') advance();
            } else if (c == '/' && peek(1) == '*') {
                Loc start{line_, col_, file_};
                advance();
                advance();
                int depth = 1;
                while (pos_ < src_.size() && depth > 0) {
                    if (peek() == '\n') mark();
                    if (peek() == '/' && peek(1) == '*') {
                        depth++;
                        advance();
                    } else if (peek() == '*' && peek(1) == '/') {
                        depth--;
                        advance();
                    }
                    advance();
                }
                if (depth > 0) diag_.error(start, "unterminated block comment");
            } else {
                break;
            }
        }
        return newline;
    }

    void lex_ident(Token& t) {
        size_t start = pos_;
        while (pos_ < src_.size() && is_ident(src_[pos_])) advance();
        t.text = src_.substr(start, pos_ - start);
        auto it = kKeywords.find(t.text);
        if (it == kKeywords.end()) {
            t.kind = Tok::Ident;
            return;
        }
        t.kind = it->second;
        if (t.kind == Tok::KwAs) {
            // as! / as% / as| harus menempel langsung pada 'as'
            if (peek() == '!') { t.kind = Tok::KwAsBang; advance(); }
            else if (peek() == '%') { t.kind = Tok::KwAsWrap; advance(); }
            else if (peek() == '|') { t.kind = Tok::KwAsSat; advance(); }
            else if (peek() == '?') { t.kind = Tok::KwAsOpt; advance(); }
        }
    }

    void lex_number(Token& t) {
        size_t start = pos_;
        Loc loc{line_, col_, file_};
        int base = 10;
        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) base = 16;
        if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) base = 2;
        if (peek() == '0' && (peek(1) == 'o' || peek(1) == 'O')) base = 8;
        if (base != 10) {
            advance();
            advance();
        }
        std::string digits;
        bool is_float = false;
        auto digit_ok = [&](char c) {
            if (base == 16) return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            if (base == 8) return c >= '0' && c <= '7';
            if (base == 2) return c == '0' || c == '1';
            return is_digit(c);
        };
        while (pos_ < src_.size() && (digit_ok(peek()) || peek() == '_')) {
            if (peek() != '_') digits += peek();
            advance();
        }
        if (base == 10) {
            // pecahan: '.' diikuti digit (bukan '..')
            if (peek() == '.' && is_digit(peek(1))) {
                is_float = true;
                digits += '.';
                advance();
                while (is_digit(peek()) || peek() == '_') {
                    if (peek() != '_') digits += peek();
                    advance();
                }
            }
            if (peek() == 'e' || peek() == 'E') {
                char n1 = peek(1);
                char n2 = peek(2);
                if (is_digit(n1) || ((n1 == '+' || n1 == '-') && is_digit(n2))) {
                    is_float = true;
                    digits += 'e';
                    advance();
                    if (peek() == '+' || peek() == '-') {
                        digits += peek();
                        advance();
                    }
                    while (is_digit(peek())) {
                        digits += peek();
                        advance();
                    }
                }
            }
        }
        if (is_ident(peek())) {
            diag_.error(Loc{line_, col_, file_}, "invalid character in number literal");
            while (is_ident(peek())) advance();
        }
        t.text = src_.substr(start, pos_ - start);
        if (digits.empty()) {
            diag_.error(loc, "number literal has no digits");
            t.kind = Tok::Int;
            return;
        }
        if (is_float) {
            t.kind = Tok::Float;
            t.float_val = std::strtod(digits.c_str(), nullptr);
            return;
        }
        t.kind = Tok::Int;
        uint64_t v = 0;
        bool overflow = false;
        for (char c : digits) {
            uint64_t d = is_digit(c) ? (uint64_t)(c - '0') : (uint64_t)((c | 0x20) - 'a' + 10);
            if (v > (UINT64_MAX - d) / (uint64_t)base) overflow = true;
            v = v * (uint64_t)base + d;
        }
        if (overflow) diag_.error(loc, "integer literal too large (max 18446744073709551615)");
        t.int_val = v;
    }

    void lex_string(Token& t) {
        Loc loc{line_, col_, file_};
        t.kind = Tok::String;
        advance();  // "
        std::string out;
        while (pos_ < src_.size() && peek() != '"') {
            char c = peek();
            if (c == '\n') break;
            if (c == '\\') {
                Loc eloc{line_, col_, file_};
                advance();
                char e = peek();
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '0': out += '\0'; break;
                    case '\\': out += '\\'; break;
                    case '"': out += '"'; break;
                    case '\'': out += '\''; break;
                    case 'x': {
                        auto hex = [](char h) -> int {
                            if (h >= '0' && h <= '9') return h - '0';
                            if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                            if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                            return -1;
                        };
                        int h1 = hex(peek(1)), h2 = hex(peek(2));
                        if (h1 < 0 || h2 < 0) {
                            diag_.error(eloc, "invalid \\x escape (expected two hex digits)");
                        } else {
                            out += (char)(h1 * 16 + h2);
                            advance();
                            advance();
                        }
                        break;
                    }
                    default: diag_.error(eloc, std::string("unknown escape sequence '\\") + e + "'");
                }
                advance();
                continue;
            }
            out += c;
            advance();
        }
        if (peek() != '"') {
            diag_.error(loc, "unterminated string literal");
        } else {
            advance();
        }
        t.text = std::move(out);
    }

    void lex_punct(Token& t) {
        char c = peek();
        char n = peek(1);
        auto one = [&](Tok k) { t.kind = k; advance(); };
        auto two = [&](Tok k) { t.kind = k; advance(); advance(); };
        switch (c) {
            case '(': one(Tok::LParen); return;
            case ')': one(Tok::RParen); return;
            case '{': one(Tok::LBrace); return;
            case '}': one(Tok::RBrace); return;
            case '[': one(Tok::LBracket); return;
            case ']': one(Tok::RBracket); return;
            case ',': one(Tok::Comma); return;
            case ';': one(Tok::Semi); return;
            case ':': if (n == '=') two(Tok::ColonEq); else one(Tok::Colon); return;
            case '#': one(Tok::Hash); return;
            case '~': one(Tok::Tilde); return;
            case '^': one(Tok::Caret); return;
            case '.': if (n == '.') two(Tok::DotDot); else one(Tok::Dot); return;
            case '+':
                if (n == '+') two(Tok::PlusPlus);
                else if (n == '?') two(Tok::PlusOpt);
                else if (n == '%') two(Tok::PlusWrap);
                else if (n == '|') two(Tok::PlusSat);
                else if (n == '=') two(Tok::PlusEq);
                else one(Tok::Plus);
                return;
            case '-':
                if (n == '-') two(Tok::MinusMinus);
                else if (n == '?') two(Tok::MinusOpt);
                else if (n == '%') two(Tok::MinusWrap);
                else if (n == '|') two(Tok::MinusSat);
                else if (n == '=') two(Tok::MinusEq);
                else if (n == '>') two(Tok::Arrow);
                else one(Tok::Minus);
                return;
            case '*':
                if (n == '?') two(Tok::StarOpt);
                else if (n == '%') two(Tok::StarWrap);
                else if (n == '|') two(Tok::StarSat);
                else if (n == '=') two(Tok::StarEq);
                else one(Tok::Star);
                return;
            case '/':
                if (n == '=') two(Tok::SlashEq);
                else if (n == '?') two(Tok::SlashOpt);
                else one(Tok::Slash);
                return;
            case '%':
                if (n == '=') two(Tok::PercentEq);
                else if (n == '?') two(Tok::PercentOpt);
                else one(Tok::Percent);
                return;
            case '?': if (n == '?') two(Tok::QQ); else one(Tok::Question); return;
            case '&': if (n == '&') two(Tok::AmpAmp); else one(Tok::Amp); return;
            case '|': if (n == '|') two(Tok::PipePipe); else one(Tok::Pipe); return;
            case '!': if (n == '=') two(Tok::NotEq); else one(Tok::Bang); return;
            case '=':
                if (n == '=') two(Tok::EqEq);
                else if (n == '>') two(Tok::FatArrow);
                else one(Tok::Eq);
                return;
            case '<':
                if (n == '-') two(Tok::LArrow);
                else if (n == '<') two(Tok::Shl);
                else if (n == '=') two(Tok::Le);
                else one(Tok::Lt);
                return;
            case '>':
                if (n == '>') two(Tok::Shr);
                else if (n == '=') two(Tok::Ge);
                else one(Tok::Gt);
                return;
            default: break;
        }
        diag_.error(Loc{line_, col_, file_}, std::string("unexpected character '") + c + "'");
        advance();
        // lanjutkan dengan token berikutnya
        Loc ignored;
        skip_space(ignored);
        t.loc = Loc{line_, col_, file_};
        if (pos_ >= src_.size()) {
            t.kind = Tok::Eof;
            return;
        }
        char c2 = src_[pos_];
        if (is_ident_start(c2)) lex_ident(t);
        else if (is_digit(c2)) lex_number(t);
        else if (c2 == '"') lex_string(t);
        else lex_punct(t);
    }

    const std::string& src_;
    const SourceFile* file_;
    Diag& diag_;
    size_t pos_ = 0;
    int line_ = 1;
    int col_ = 1;
};

}  // namespace

std::vector<Token> lex(const SourceFile& file, Diag& diag) {
    Lexer lx(file, diag);
    return lx.run();
}

}  // namespace tl
