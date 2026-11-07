#include "range.hpp"

#include <initializer_list>

namespace tl {

Big Big::of(bool neg, uint64_t mag) {
    Big b;
    b.mag = mag;
    b.neg = neg && mag != 0;
    return b;
}

Big Big::of_i64(int64_t v) {
    if (v >= 0) return of(false, (uint64_t)v);
    return of(true, (uint64_t)(-(v + 1)) + 1);  // aman untuk INT64_MIN
}

Big Big::of_u64(uint64_t v) { return of(false, v); }

std::string Big::str() const {
    if (k == NegInf) return "-inf";
    if (k == PosInf) return "+inf";
    return (neg ? "-" : "") + std::to_string(mag);
}

int cmp(const Big& a, const Big& b) {
    auto rank = [](const Big& x) { return x.k == Big::NegInf ? 0 : x.k == Big::Fin ? 1 : 2; };
    int ra = rank(a), rb = rank(b);
    if (ra != rb) return ra < rb ? -1 : 1;
    if (ra != 1) return 0;
    bool an = a.is_negative(), bn = b.is_negative();
    if (an != bn) return an ? -1 : 1;
    if (a.mag == b.mag) return 0;
    bool less_mag = a.mag < b.mag;
    // keduanya negatif: magnitudo lebih besar = lebih kecil
    if (an) return less_mag ? 1 : -1;
    return less_mag ? -1 : 1;
}

Big bneg(const Big& a) {
    if (a.k == Big::NegInf) return Big::pos_inf();
    if (a.k == Big::PosInf) return Big::neg_inf();
    return Big::of(!a.neg, a.mag);
}

Big babs(const Big& a) {
    if (!a.is_fin()) return Big::pos_inf();
    return Big::of(false, a.mag);
}

Big badd(const Big& a, const Big& b) {
    if (!a.is_fin()) return a;
    if (!b.is_fin()) return b;
    if (a.neg == b.neg) {
        if (a.mag > UINT64_MAX - b.mag) return a.neg ? Big::neg_inf() : Big::pos_inf();
        return Big::of(a.neg, a.mag + b.mag);
    }
    if (a.mag >= b.mag) return Big::of(a.neg, a.mag - b.mag);
    return Big::of(b.neg, b.mag - a.mag);
}

Big bsub(const Big& a, const Big& b) { return badd(a, bneg(b)); }

Big bmul(const Big& a, const Big& b) {
    if (a.is_zero() || b.is_zero()) return Big::of_u64(0);
    bool neg = a.is_negative() != b.is_negative();
    if (!a.is_fin() || !b.is_fin()) return neg ? Big::neg_inf() : Big::pos_inf();
    if (a.mag > UINT64_MAX / b.mag) return neg ? Big::neg_inf() : Big::pos_inf();
    return Big::of(neg, a.mag * b.mag);
}

Big bdiv_trunc(const Big& a, const Big& b) {
    bool neg = a.is_negative() != b.is_negative();
    if (!b.is_fin()) return Big::of_u64(0);
    if (!a.is_fin()) return neg ? Big::neg_inf() : Big::pos_inf();
    if (b.mag == 0) return Big::of_u64(0);  // tidak terjadi: pemanggil menjamin b != 0
    return Big::of(neg, a.mag / b.mag);
}

Big bshr_floor(const Big& a, unsigned k) {
    if (!a.is_fin()) return a;
    if (!a.is_negative()) return Big::of(false, k >= 64 ? 0 : a.mag >> k);
    if (k >= 64) return Big::of(true, 1);
    return Big::of(true, ((a.mag - 1) >> k) + 1);
}

Big bshl(const Big& a, unsigned k) {
    if (!a.is_fin() || a.is_zero()) return a;
    if (k >= 64 || a.mag > (UINT64_MAX >> k)) return a.neg ? Big::neg_inf() : Big::pos_inf();
    return Big::of(a.neg, a.mag << k);
}

bool Interval::contains(const Big& v) const {
    if (excl_zero && v.is_zero()) return false;
    return lo <= v && v <= hi;
}

void Interval::normalize() {
    if (!excl_zero) return;
    if (lo.is_zero()) lo = Big::of_u64(1);
    if (hi.is_zero()) hi = Big::of(true, 1);
    if (!lo.is_negative() || hi.is_negative()) excl_zero = false;  // 0 di luar [lo, hi]
}

std::string Interval::str() const {
    std::string s = "[" + lo.str() + ", " + hi.str() + "]";
    if (excl_zero) s += " excluding 0";
    return s;
}

Interval type_range(const Type* t) {
    if (!t || !t->is_int()) return {Big::neg_inf(), Big::pos_inf(), false};
    if (t->is_signed) {
        uint64_t half = 1ull << (t->bits - 1);
        return {Big::of(true, half), Big::of(false, half - 1), false};
    }
    uint64_t max = t->bits == 64 ? UINT64_MAX : (1ull << t->bits) - 1;
    return {Big::of_u64(0), Big::of_u64(max), false};
}

bool fits(const Interval& r, const Type* t) {
    if (r.empty()) return true;
    Interval tr = type_range(t);
    return tr.lo <= r.lo && r.hi <= tr.hi;
}

Interval hull(const Interval& a, const Interval& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    Interval r{bmin(a.lo, b.lo), bmax(a.hi, b.hi), !a.contains_zero() && !b.contains_zero()};
    r.normalize();
    return r;
}

Interval intersect(const Interval& a, const Interval& b) {
    Interval r{bmax(a.lo, b.lo), bmin(a.hi, b.hi), a.excl_zero || b.excl_zero};
    if (!r.empty()) r.normalize();
    return r;
}

Interval clamp_to(const Interval& r, const Type* t) {
    Interval tr = type_range(t);
    auto clamp = [&](const Big& v) { return bmin(bmax(v, tr.lo), tr.hi); };
    return {clamp(r.lo), clamp(r.hi), false};
}

static Interval from_points(std::initializer_list<Big> pts) {
    Interval r{*pts.begin(), *pts.begin(), false};
    for (const Big& p : pts) {
        r.lo = bmin(r.lo, p);
        r.hi = bmax(r.hi, p);
    }
    return r;
}

Interval iadd(const Interval& a, const Interval& b) { return {badd(a.lo, b.lo), badd(a.hi, b.hi), false}; }

Interval isub(const Interval& a, const Interval& b) { return {bsub(a.lo, b.hi), bsub(a.hi, b.lo), false}; }

Interval imul(const Interval& a, const Interval& b) {
    return from_points({bmul(a.lo, b.lo), bmul(a.lo, b.hi), bmul(a.hi, b.lo), bmul(a.hi, b.hi)});
}

Interval ineg(const Interval& a) {
    Interval r{bneg(a.hi), bneg(a.lo), a.excl_zero};
    return r;
}

Interval idiv(const Interval& a, const Interval& b) {
    Interval r{Big::pos_inf(), Big::neg_inf(), false};  // kosong
    auto part = [&](const Big& lo, const Big& hi) {
        Interval q = from_points({bdiv_trunc(a.lo, lo), bdiv_trunc(a.lo, hi), bdiv_trunc(a.hi, lo),
                                  bdiv_trunc(a.hi, hi)});
        r = r.empty() ? q : hull(r, q);
    };
    Big one = Big::of_u64(1), minus_one = Big::of(true, 1);
    if (b.lo.is_negative()) part(b.lo, bmin(b.hi, minus_one));
    if (Big::of_u64(0) < b.hi) part(bmax(b.lo, one), b.hi);
    if (a.lo <= Big::of_u64(0) && Big::of_u64(0) <= a.hi) r = hull(r, Interval::exact(Big::of_u64(0)));
    r.excl_zero = false;
    return r;
}

Interval irem(const Interval& a, const Interval& b) {
    Big m = bsub(bmax(babs(b.lo), babs(b.hi)), Big::of_u64(1));  // |hasil| <= |b| - 1
    Big zero = Big::of_u64(0);
    if (!a.lo.is_negative()) return {zero, bmin(a.hi, m), false};
    if (a.hi <= zero) return {bmax(a.lo, bneg(m)), zero, false};
    return {bmax(a.lo, bneg(m)), bmin(a.hi, m), false};
}

Interval ishl(const Interval& a, const Interval& k) {
    unsigned k0 = (unsigned)k.lo.mag, k1 = (unsigned)k.hi.mag;
    return from_points({bshl(a.lo, k0), bshl(a.lo, k1), bshl(a.hi, k0), bshl(a.hi, k1)});
}

Interval ishr(const Interval& a, const Interval& k) {
    unsigned k0 = (unsigned)k.lo.mag, k1 = (unsigned)k.hi.mag;
    return from_points({bshr_floor(a.lo, k0), bshr_floor(a.lo, k1), bshr_floor(a.hi, k0),
                        bshr_floor(a.hi, k1)});
}

Interval ibitand(const Interval& a, const Interval& b, const Type* t) {
    bool an = !a.lo.is_negative(), bn = !b.lo.is_negative();
    Big zero = Big::of_u64(0);
    if (an && bn) return {zero, bmin(a.hi, b.hi), false};
    if (an) return {zero, a.hi, false};
    if (bn) return {zero, b.hi, false};
    return type_range(t);
}

Interval ibitor(const Interval& a, const Interval& b, const Type* t) {
    if (a.lo.is_negative() || b.lo.is_negative() || !a.hi.is_fin() || !b.hi.is_fin()) return type_range(t);
    uint64_t m = bmax(a.hi, b.hi).mag;
    uint64_t p = 0;
    while (p < m) p = (p << 1) | 1;  // 2^n - 1 terkecil yang >= m
    return {Big::of_u64(0), Big::of_u64(p), false};
}

}  // namespace tl
