#pragma once

// Prelude tree-lang: fungsi generik untuk array, ditulis dalam tree-lang sendiri.
// Ikut dikompilasi ke setiap program; fungsi yang tidak dipakai tidak menghasilkan kode
// (template hanya diinstansiasi saat dipanggil). Program boleh mendefinisikan fungsi
// dengan nama yang sama untuk menggantikannya.
namespace tl {

inline constexpr const char* kPreludePath = "<prelude>";

inline constexpr const char* kPrelude = R"TREE(
// xs.each() { print(it) }
fn each[T](xs []T, f fn(T)) {
    for _, x := range xs { f(x) }
}

// xs.filter() { it % 2 == 0 }
fn filter[T](xs []T, keep fn(T) bool) [..]T {
    var out [..]T
    for _, x := range xs {
        if keep(x) { out.push(x) }
    }
    return out
}

// xs.map() { it * 2 }
fn map[T, U](xs []T, f fn(T) U) [..]U {
    var out [..]U
    for _, x := range xs { out.push(f(x)) }
    return out
}

// xs.fold(0) { acc, x => acc +| x }
fn fold[T, A](xs []T, init A, f fn(A, T) A) A {
    var acc = init
    for _, x := range xs { acc = f(acc, x) }
    return acc
}

// xs.count() { it > 3 }
fn count[T](xs []T, pred fn(T) bool) i64 {
    var n i64
    for _, x := range xs {
        if pred(x) { n++ }
    }
    return n
}

fn any[T](xs []T, pred fn(T) bool) bool {
    for _, x := range xs {
        if pred(x) { return true }
    }
    return false
}

fn all[T](xs []T, pred fn(T) bool) bool {
    for _, x := range xs {
        if !pred(x) { return false }
    }
    return true
}

// indeks elemen pertama yang memenuhi, atau none
fn find[T](xs []T, pred fn(T) bool) i64? {
    for i, x := range xs {
        if pred(x) { return i }
    }
    return none
}

// sort(&xs) { a, b => a < b }   (insertion sort, stabil; semua indeks terbukti aman)
fn sort[T](xs &[..]T, less fn(T, T) bool) {
    for i := range 1..len(xs) {
        var j = i
        for j > 0 && less(xs[j], xs[j - 1]) {
            t := xs[j]
            xs[j] = xs[j - 1]
            xs[j - 1] = t
            j--
        }
    }
}

fn min[T](a, b T) T {
    if b < a { return b }
    return a
}

fn max[T](a, b T) T {
    if b > a { return b }
    return a
}

fn equal[T](a, b []T) bool {
    if len(a) != len(b) { return false }
    for i, x := range a {
        if i < len(b) {
            if x != b[i] { return false }
        }
    }
    return true
}

fn index_of[T](xs []T, x T) i64? {
    for i, y := range xs {
        if y == x { return i }
    }
    return none
}

fn contains[T](xs []T, x T) bool {
    for _, y := range xs {
        if y == x { return true }
    }
    return false
}

// ---------------- stdlib OS: pembungkus aman di atas runtime C ----------------
// Semua kegagalan dilaporkan lewat bool / T?, tidak pernah lewat panic.
unsafe extern fn tl_os_read_file(path []u8, out &[..]u8) bool
unsafe extern fn tl_os_write_file(path []u8, data []u8) bool
unsafe extern fn tl_os_append_file(path []u8, data []u8) bool
unsafe extern fn tl_os_file_exists(path []u8) bool
unsafe extern fn tl_os_delete_file(path []u8) bool
unsafe extern fn tl_os_read_line(out &[..]u8) bool
unsafe extern fn tl_os_arg_count() i64
unsafe extern fn tl_os_arg(i i64, out &[..]u8) bool
unsafe extern fn tl_os_env(name []u8, out &[..]u8) bool
unsafe extern fn tl_os_time_ns() i64
unsafe extern fn tl_os_clock_ns() i64
unsafe extern fn tl_os_sleep_ms(ms i64)
unsafe extern fn tl_os_random_seed() u64
unsafe extern fn tl_os_write_stderr(s []u8)
unsafe extern fn tl_parse_int(s []u8, out &i64) bool
unsafe extern fn tl_parse_float(s []u8, out &f64) bool

// isi 'out' dengan seluruh isi file; false bila gagal dibaca
fn read_file(path []u8, out &[..]u8) bool { return tl_os_read_file(path, out) }
fn write_file(path []u8, data []u8) bool { return tl_os_write_file(path, data) }
fn append_file(path []u8, data []u8) bool { return tl_os_append_file(path, data) }
fn file_exists(path []u8) bool { return tl_os_file_exists(path) }
fn delete_file(path []u8) bool { return tl_os_delete_file(path) }
// satu baris dari stdin (tanpa newline); false di akhir input
fn read_line(out &[..]u8) bool { return tl_os_read_line(out) }
fn arg_count() i64 { return tl_os_arg_count() }
fn arg(i i64, out &[..]u8) bool { return tl_os_arg(i, out) }
fn env(name []u8, out &[..]u8) bool { return tl_os_env(name, out) }
// nanodetik sejak 1970 (jam dinding) / sejak titik tetap (monoton, untuk mengukur durasi)
fn time_ns() i64 { return tl_os_time_ns() }
fn clock_ns() i64 { return tl_os_clock_ns() }
fn sleep_ms(ms i64) { tl_os_sleep_ms(ms) }
fn random_seed() u64 { return tl_os_random_seed() }
fn write_stderr(s []u8) { tl_os_write_stderr(s) }

fn parse_int(s []u8) i64? {
    var v i64
    if tl_parse_int(s, &v) { return v }
    return none
}

fn parse_float(s []u8) f64? {
    var v f64
    if tl_parse_float(s, &v) { return v }
    return none
}
)TREE";

}  // namespace tl
