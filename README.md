# tree-lang

Bahasa sistem yang memory-safe dengan konsep dari [Goose](https://github.com/aardappel/goose): tanpa heap, tanpa GC, tanpa anotasi lifetime, dan **ketat saat compile**. Sintaksnya sederhana seperti Go. Compiler ditulis dalam C++20 dan menghasilkan LLVM IR.

```
type Point struct {
    x, y i64
}

fn (p Point) sum() i64 {
    if p.x < 0 || p.x > 1000 || p.y < 0 || p.y > 1000 { return 0 }
    return p.x + p.y
}

fn divmod(a, b u32) (u32, u32) {
    if b == 0 {
        return 0, 0 // wajib: tanpa ini compile error "possible division by zero"
    }
    return a / b, a % b
}

fn main() {
    var total i64
    for i := range 1..101 { total += i } // terbukti tidak overflow (analisis induksi)
    q, r := divmod(17, 5)
    print("total ", total, ", 17/5 = ", q, " sisa ", r, ", ", Point{1, 2}.sum())
}
```

Package belum ada: kata `import` dicadangkan untuk package manager resmi yang akan datang.

## Prasyarat (Windows)
- Visual Studio 2022+ (MSVC, CMake bawaan VS)
- LLVM/clang: `winget install LLVM.LLVM`
- Python 3 (untuk test)

## Build & jalankan
```powershell
.\build.ps1                 # build compiler → build\Release\treelang.exe
.\build.ps1 -Test           # build + jalankan semua test
build\Release\treelang.exe run samples\01_halo.tree
build\Release\treelang.exe build samples\01_halo.tree     # → samples\01_halo.exe
build\Release\treelang.exe check file.tree                # hanya pemeriksaan + pembuktian
build\Release\treelang.exe emit file.tree -o file.ll      # lihat LLVM IR
```
Opsi: `-O0`, `--debug` (verifikasi ulang semua bukti saat runtime), `-v`.

## Struktur
```
src/lexer, parser, ast, types   frontend
src/sema/check.cpp              tipe, inisialisasi, must-use, pembuktian rentang
src/sema/range.cpp              aritmetika interval (presisi penuh i64/u64)
src/codegen_llvm.cpp            AST → LLVM IR
src/prelude.hpp                 pustaka generik bawaan (ditulis dalam tree-lang)
src/toolchain.cpp               memanggil clang
runtime/tl_runtime.c            print, abort, region data, serialisasi, thread & queue, OS
editors/vscode/                 syntax highlighting
test/ok, test/err               program yang harus lulus / harus ditolak
docs/spec.md                    spesifikasi bahasa (termasuk array & model memori)
```

## Status milestone
- [x] **M0** kerangka, CLI, pipeline LLVM, test runner
- [x] **M1** inti bahasa + mode ketat (overflow, bagi nol, shift, cast, init, must-use, return) + sintaks ala Go
- [x] **M2** array `[N]T`, `[..]T`, slice `[]T`, string; region data per array (tanpa heap); indeks terbukti saat compile
- [x] **M3** referensi `&T` (parameter, receiver, `r := &x`), struct berisi `[..]T`, slicing `xs[a..b]`; anti-dangling & anti-alias dicek saat compile
- [x] **M4** enum dengan payload, `match` yang wajib lengkap (enum/optional/int/bool), optional `T?` + `if some`, `??`, `get`, `as?`, `+? -? *? /? %?`
- [x] **M5** fungsi generik `fn f[T]` (inferensi + monomorfisasi), parameter fungsi `fn(T) U` non-escaping, trailing block `xs.filter() { it > 0 }` (di-inline), prelude `filter/map/fold/sort/...`
- [x] **M6** `return e from f` lintas frame (juga non-local return dari block); keberadaan `f` yang aktif dibuktikan lewat graf pemanggilan; tanpa unwinder, tanpa biaya bagi pemanggilan lain
- [x] **M7** `Pool[T]`/`Handle[T]` generasional (handle basi → none), `Link[T]` (offset u32 relatif), `to_bytes`/`from_bytes` dengan validasi penuh
- [x] **M8** goroutine `run f(...)` + channel `chan[T](n)`, `<-`, `select`, `close`, `wait` (scheduler M:N, tanpa memori bersama → tanpa data race, move `[..]T` lewat channel, deteksi deadlock)
- [x] **M9** `unsafe extern fn` (FFI C + sidecar `.c`), stdlib OS (file/env/arg/waktu/`str`/`parse_int`), `#max_depth(N)` + analisis stack native, `--unsafe-report`/`--panic-report`, highlighting VS Code
- [x] **Package** ala Go: `forest.json` (`{"module": "myapp"}`), satu folder = satu package, `import "myapp/geo"` / alias / `import ( ... )`, ekspor huruf besar; import tak terpakai, siklik, dan shadowing ditolak saat compile
- [x] **defer / panic / recover** ala Go: `defer f(x)` / `defer { ... }` (LIFO, tidak di loop), `panic("msg")`, `if some msg := recover()` di defer; `#max_depth` dan kehabisan memori menjadi panic yang bisa di-recover; `abort` tetap fatal; tanpa biaya bila tidak dipakai
- [ ] M10 benchmark vs C dan Rust
