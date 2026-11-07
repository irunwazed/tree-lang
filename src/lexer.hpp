#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "diag.hpp"

namespace tl {

enum class Tok {
    Eof,
    Ident,
    Int,
    Float,
    String,
    // keywords
    KwFn, KwVar, KwConst, KwStruct, KwType, KwIf, KwElse, KwFor, KwRange,
    KwBreak, KwContinue, KwReturn, KwImport, KwTrue, KwFalse,
    KwEnum, KwMatch, KwNone, KwSome,
    KwAs, KwAsBang, KwAsWrap, KwAsSat, KwAsOpt,  // as, as!, as%, as|, as?
    // punctuation
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Semi, Colon, ColonEq, Dot, DotDot, Arrow, Hash,
    // operators
    Plus, Minus, Star, Slash, Percent,
    PlusWrap, MinusWrap, StarWrap,  // +% -% *%
    PlusSat, MinusSat, StarSat,     // +| -| *|
    PlusOpt, MinusOpt, StarOpt, SlashOpt, PercentOpt,  // +? -? *? /? %?  (hasil T?)
    Question, QQ,                   // T?   a ?? b
    FatArrow,                       // =>  (parameter trailing block)
    LArrow,                         // <-  (kirim / terima channel)
    Amp, Pipe, Caret, Tilde, Shl, Shr,
    AmpAmp, PipePipe, Bang,
    Eq, EqEq, NotEq, Lt, Le, Gt, Ge,
    PlusEq, MinusEq, StarEq, SlashEq, PercentEq,
    PlusPlus, MinusMinus,
};

struct Token {
    Tok kind = Tok::Eof;
    Loc loc;
    std::string text;      // identifier / decoded string literal / raw number text
    bool auto_semi = false;  // ';' hasil penyisipan otomatis di akhir baris
    uint64_t int_val = 0;  // Int
    double float_val = 0;  // Float
};

const char* tok_name(Tok t);

std::vector<Token> lex(const SourceFile& file, Diag& diag);

}  // namespace tl
