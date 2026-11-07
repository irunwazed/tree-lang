#pragma once
#include <cstdint>
#include <string>

#include "../types.hpp"

namespace tl {

// Bilangan bulat untuk batas interval: tanda + magnitudo 64-bit, atau ±tak hingga.
// Mencakup [-(2^64-1), 2^64-1] sehingga semua nilai i64 dan u64 terwakili tepat.
struct Big {
    enum Kind : uint8_t { Fin, NegInf, PosInf };
    Kind k = Fin;
    bool neg = false;  // hanya bermakna jika Fin dan mag != 0
    uint64_t mag = 0;

    static Big of_i64(int64_t v);
    static Big of_u64(uint64_t v);
    static Big of(bool neg, uint64_t mag);
    static Big neg_inf() { Big b; b.k = NegInf; return b; }
    static Big pos_inf() { Big b; b.k = PosInf; return b; }

    bool is_fin() const { return k == Fin; }
    bool is_zero() const { return k == Fin && mag == 0; }
    bool is_negative() const { return k == NegInf || (k == Fin && neg && mag != 0); }
    std::string str() const;
};

int cmp(const Big& a, const Big& b);
inline bool operator<(const Big& a, const Big& b) { return cmp(a, b) < 0; }
inline bool operator<=(const Big& a, const Big& b) { return cmp(a, b) <= 0; }
inline bool operator==(const Big& a, const Big& b) { return cmp(a, b) == 0; }
inline const Big& bmin(const Big& a, const Big& b) { return a < b ? a : b; }
inline const Big& bmax(const Big& a, const Big& b) { return a < b ? b : a; }

Big badd(const Big& a, const Big& b);
Big bneg(const Big& a);
Big bsub(const Big& a, const Big& b);
Big bmul(const Big& a, const Big& b);
Big bdiv_trunc(const Big& a, const Big& b);    // b != 0
Big bshr_floor(const Big& a, unsigned k);      // a >> k (aritmetik, pembulatan ke -inf)
Big bshl(const Big& a, unsigned k);            // a * 2^k
Big babs(const Big& a);

// Himpunan nilai integer [lo, hi]; excl_zero = 0 dikecualikan walau lo <= 0 <= hi.
struct Interval {
    Big lo, hi;
    bool excl_zero = false;

    static Interval exact(const Big& v) { return {v, v, false}; }
    static Interval of(const Big& lo, const Big& hi) { return {lo, hi, false}; }
    bool contains(const Big& v) const;
    bool contains_zero() const { return contains(Big::of_u64(0)); }
    bool is_singleton() const { return lo == hi; }
    bool empty() const { return hi < lo; }
    void normalize();  // jadikan excl_zero eksplisit di batas jika memungkinkan
    std::string str() const;
};

Interval type_range(const Type* t);  // rentang penuh tipe integer
bool fits(const Interval& r, const Type* t);
Interval hull(const Interval& a, const Interval& b);
Interval intersect(const Interval& a, const Interval& b);
Interval clamp_to(const Interval& r, const Type* t);

// Aritmetika interval matematis (tanpa batas tipe). Hasil bisa melebihi tipe:
// pemanggil yang mengecek fits() untuk membuktikan tidak ada overflow.
Interval iadd(const Interval& a, const Interval& b);
Interval isub(const Interval& a, const Interval& b);
Interval imul(const Interval& a, const Interval& b);
Interval ineg(const Interval& a);
Interval idiv(const Interval& a, const Interval& b);  // b tidak memuat 0
Interval irem(const Interval& a, const Interval& b);  // b tidak memuat 0
Interval ishl(const Interval& a, const Interval& k);  // k dalam [0, bits)
Interval ishr(const Interval& a, const Interval& k);
Interval ibitand(const Interval& a, const Interval& b, const Type* t);
Interval ibitor(const Interval& a, const Interval& b, const Type* t);  // | dan ^

}  // namespace tl
