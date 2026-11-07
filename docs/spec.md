# Spesifikasi tree-lang (v0.3, Milestone M1, sintaks ala Go)

tree-lang adalah bahasa sistem yang memory-safe dengan konsep dari Goose: tanpa heap, tanpa GC, dan tanpa anotasi lifetime. Sintaksnya **sederhana seperti Go**: tanpa class, tanpa inheritance, tanpa titik koma.

Prinsip utama: **ketat saat compile**. Setiap operasi yang bisa gagal di runtime harus *dibuktikan aman* oleh compiler, atau ditangani eksplisit lewat tipe. Kalau tidak, program ditolak. Program yang lolos compile tidak punya panic implisit.

Status: M1 sampai M9 plus package (lihat README). Program bisa satu file (mode skrip) atau modul multi-package dengan `forest.json` (bagian 6j). Package eksternal akan tersedia lewat package manager resmi.

## 1. Contoh
```
type Point struct {
    x, y i64
}

const LIMIT = 10

fn (p Point) area() i64 {
    if p.x < 0 || p.x > 1000 || p.y < 0 || p.y > 1000 {
        return 0
    }
    return p.x * p.y
}

fn divmod(a, b u32) (u32, u32) {
    if b == 0 {
        return 0, 0
    }
    return a / b, a % b
}

fn main() {
    p := Point{x: 3, y: 4}
    q, r := divmod(17, 5)
    var total i64
    for i := range 1..LIMIT + 1 {
        total += i
    }
    print(p.area(), " ", q, " ", r, " ", total)
}
```

## 2. Leksikal
- Komentar: `// baris` dan `/* blok, boleh bersarang */`.
- **Akhir statement = akhir baris.** Lexer menyisipkan `;` di akhir baris jika token terakhir adalah identifier, literal, `break`/`continue`/`return`/`true`/`false`, `)`, `]`, `}`, `++`, atau `--` (aturan Go). `;` boleh ditulis untuk memisahkan beberapa statement di satu baris.
  - Akibatnya, `{` harus di baris yang sama dengan `fn`/`if`/`for`, dan `} else {` harus satu baris.
  - Ekspresi boleh dilanjutkan ke baris berikutnya setelah operator atau koma: `a +⏎ b`, `f(⏎ x,⏎ y,⏎ )`.
- Integer: `42`, `1_000_000`, `0xff`, `0b1010`, `0o17`. Float: `1.5`, `1e9`.
- String: `"teks\n"`, dengan escape `\n \t \r \0 \\ \" \' \xHH`. Di M1, string hanya bisa dipakai di `print`/`abort`.
- Kata kunci: `fn var const type struct if else for range break continue return import true false as`.

## 3. Tipe
| Tipe | Keterangan |
|---|---|
| `i8 i16 i32 i64` | integer bertanda |
| `u8 u16 u32 u64` | integer tak bertanda |
| `f32 f64` | IEEE-754 |
| `bool` | `true` / `false` |
| `type T struct { ... }` | nilai inline, disalin saat assignment |

**Tidak ada konversi implisit** (`i32 + i64` adalah error). Literal tanpa tipe mengikuti tipe pasangannya, dengan default `i64`/`f64`. Literal yang tidak muat di tipenya adalah error.

**Nilai nol:** `var x T` diisi 0, 0.0, false, atau struct yang semua field-nya nol.

## 4. Deklarasi top level
```
type Point struct {
    x, y i64       // field dikelompokkan seperti Go
    id   u32
}
const LIMIT = 10           // const NAME [T] = literal
const MASK u8 = 0xff

fn add(a, b i32) i32 { ... }             // parameter berkelompok
fn divmod(a, b u32) (u32, u32) { ... }   // multi-return
fn (p Point) area() i64 { ... }          // method (receiver)
fn main() { ... }                        // wajib ada
```
- `return` selalu ditulis eksplisit (tidak ada return implisit).
- **Method**: fungsi dengan receiver, dipanggil `p.area()`. Tidak ada class, constructor, atau inheritance.
  - Nama method berlaku per tipe, jadi `Rect.area` dan `Square.area` boleh sama-sama ada.
  - Receiver diterima by value (salinan). Method yang mengubah receiver datang di M3 bersama referensi.
  - Method hanya untuk struct, dan nama method tidak boleh sama dengan nama field.

## 5. Statement
```
x := 5                  // deklarasi singkat (tipe dari nilai)
a, b := divmod(7, 2)    // destrukturisasi multi-return; '_' untuk membuang
var y i32               // nilai nol
var z i64 = 1
var w = 2.5
y = 2;  y += 1;  y++;  y--;  p.x -= 3
_ = f()                 // membuang hasil secara eksplisit

if c { } else if d { } else { }
for i := range 10 { }       // 0..9
for i := range 2..8 { }     // 2..7
for cond { }                // seperti while
for { }                     // loop tanpa akhir; keluar dengan break/return
break   continue   return a, b
```
- Semua variabel bisa diubah. Hanya `const` yang tetap.
- Parameter adalah salinan lokal dan boleh diubah.
- Tidak ada `guard`. Pakai `if !ok { return ... }`. Setelah `if` yang cabangnya selalu keluar, compiler tahu kondisinya salah:
  ```
  if b == 0 { return 0 }
  return a / b           // aman: b terbukti != 0
  ```

## 6. Operator
- Aritmetika: `+ - * / %`. Untuk integer, **harus terbukti tidak overflow, tidak membagi nol, dan tidak `MIN / -1`.** Ini juga berlaku untuk `+=` dan `x++`.
- Wrap (eksplisit): `+% -% *%`. Saturasi: `+| -| *|`.
- Bitwise: `& | ^ ~ << >>`. Jumlah shift harus terbukti berada di `0..bits-1`. `<<` pada tipe signed juga tidak boleh overflow.
- Logika: `&& || !` (short-circuit). Operan kanan diperiksa dengan pengetahuan dari operan kiri, jadi `b > 0 && a / b > 2` aman.
- Perbandingan: `== != < <= > >=`, tidak bisa dirantai.
- Cast:
  - `as`: hanya konversi lossless;
  - `as!`: menyempit, **harus terbukti muat**;
  - `as%`: wrap/potong;
  - `as|`: saturasi. Float→int hanya lewat `as|` (NaN → 0).

## 6b. Array (M2)
| Tipe | Arti | Hidup di |
|---|---|---|
| `[N]T` | array ukuran tetap, nilai (disalin saat assignment) | stack native, dibatasi 64 KiB per nilai |
| `[..]T` | array **grow-only** (seperti `Vec`, tapi tidak pernah berpindah) | region data miliknya sendiri |
| `[]T` | slice **baca-saja** `{ptr, len}` | hanya sebagai parameter |

```
a := [3]i64{1, 2, 3}        var z [4]i64              // nilai nol
var xs [..]i64              xs.push(5)                xs.append(a)
ys := copy(xs)              s := "teks"               // string = [..]u8
for i, x := range xs { }    for i := range len(xs) { }
fn sum(v []i64) i64 { ... } sum(a); sum(xs)           // array apa pun → slice
```

**Indeks harus terbukti dalam batas saat compile.** Bukti diterima dari:
- rentang indeks lebih kecil dari panjang yang diketahui (`[N]T`, atau `if len(xs) > 9 { xs[9] }`);
- fakta `i < len(xs)` dari `if i < len(xs)`, `for i := range len(xs)`, atau `for i, x := range xs`.

Karena `[..]T` hanya bisa tumbuh, fakta itu **tetap benar walau ada `push` sesudahnya**. Mengubah `i` atau mengganti `xs` membatalkan fakta tersebut. Setelah `for i := range 500 { xs.push(i) }`, compiler tahu `len(xs) == 500` (analisis induksi). Nilai elemen tidak dilacak, jadi `total += xs[i]` perlu pembuktian atau operator `+|` / `+%`.

**Model memori (konsep Goose, tanpa heap allocator):**
- Setiap `[..]T` memiliki satu region alamat virtual. Region dicadangkan sekali (default 4 GiB, ubah dengan env `TL_RESERVE_MB`) lalu di-commit bertahap.
- Data **tidak pernah berpindah** saat tumbuh.
- Region dibebaskan otomatis saat scope ditinggalkan (akhir blok, `break`, `continue`, `return`) dan dikembalikan ke pool untuk dipakai ulang.

Aturan kepemilikan, agar setiap region punya tepat satu pemilik:
- `return xs` **memindahkan** array ke pemanggil (tanpa salinan).
- `ys := xs` ditolak. Gunakan `copy(xs)`.
- Array tumbuh baru (hasil fungsi, literal, `copy`) harus langsung disimpan ke variabel, di-`return`, atau dibuang dengan `_ =`.
- `[..]T` tidak boleh menjadi field struct atau elemen array, dan diteruskan ke fungsi sebagai `[]T`. Akses yang bisa mengubah datang di M3 bersama referensi.
- Slice tidak bisa disimpan atau di-return.

Cetak: `[..]u8` / `[]u8` dicetak sebagai teks, array lain sebagai `[1, 2, 3]`.
Error runtime yang tersisa di M2 hanya kehabisan memori atau ruang alamat (`abort`, exit 101).

## 6c. Referensi (M3): aman tanpa anotasi lifetime
```
fn bump(n &u8) { if n < 255 { n++ } }        // parameter referensi; di dalam fungsi: auto-deref
bump(&c)                                      // pemanggil menulis '&': perubahan terlihat di call site

type Stack struct { items [..]i64 }           // struct boleh memiliki array tumbuh
fn (s &Stack) push(v i64) { s.items.push(v) } // receiver &T; dipanggil 's.push(1)' (otomatis dipinjam)

r := &xs[0]                                   // referensi lokal; root (xs) diinferensi
total(xs[1..3])                               // slicing: tampilan baca-saja, batas terbukti
```
Aturan yang dicek saat compile (semuanya mencegah referensi menggantung):
1. **Root yang sedang dipinjam tidak boleh diganti.** Selama `r := &xs[i]` masih dalam scope, atau selama `for ... range xs` berjalan, `xs = ...` dan `f(&xs)` ditolak. `xs.push(...)` tetap boleh karena data tidak pernah berpindah.
2. **Satu pemanggilan tidak boleh menerima dua referensi ke root yang sama** (`swap(&x, &x)`), dan tidak boleh menerima `&xs` bersamaan dengan slice dari `xs`. Karena itu tidak ada aliasing tersembunyi, dan analisis rentang di dalam fungsi tetap sound.
3. **Referensi hanya hidup di parameter dan `r := &x`.** Referensi tidak bisa disimpan di struct atau array, di-return, atau diganti targetnya.
4. Setelah `f(&x)` atau setelah menulis lewat referensi lokal, compiler **melupakan** rentang dan fakta tentang `x`, karena nilainya mungkin sudah berubah.

Nilai yang memiliki array tumbuh (`[..]T`, atau struct yang berisi `[..]T`):
- dibebaskan otomatis secara rekursif;
- dipindahkan saat `return`;
- tidak pernah disalin diam-diam;
- diteruskan ke fungsi sebagai `&T` (atau `[]T` untuk baca saja).

## 6d. Enum, `match`, dan optional (M4)
```
type Shape enum {
    Circle { r f64 }
    Rect { w, h f64 }
    Dot
}
s := Shape.Circle{r: 2.0}       d := Shape.Dot
fn (s Shape) area() f64 { ... } // method juga boleh untuk enum

match s {
    Circle(c) { print(c.r) }    // c = salinan payload
    Rect(&r) { r.w = 10.0 }     // &r = ubah payload di tempat (s dipinjam selama cabang ini)
    Dot { }
}
match n { 0 { } 1, 2 { } _ { } }   // integer/bool: n dipersempit di setiap cabang
```
- `match` **harus lengkap**: setiap varian, `some`/`none`, atau `true`/`false` harus ditangani, atau ada `_`. Untuk integer, `_` wajib.
- Enum disimpan sebagai tag ditambah ruang untuk payload terbesar. Payload enum belum boleh memiliki `[..]T`.

**Optional `T?`: tidak ada null dan tidak ada unwrap paksa.**
```
var age u8? = none            fn find(xs []i64, x i64) i64? { ... return i ... return none }
if some i := find(xs, 7) { }   match o { some(x) { } none { } }
v := o ?? 0                    // nilai default
```
Nilai `T` otomatis menjadi `some` di tempat yang mengharapkan `T?`. Nilai `T?` tidak bisa dipakai sebagai `T` tanpa `if some`, `match`, atau `??`.

Operasi yang menghasilkan `none` alih-alih gagal:

| Operasi | Hasil |
|---|---|
| `xs.get(i)` | `T?`: `none` bila di luar batas, tanpa perlu bukti |
| `x as? u8` | `none` bila tidak muat (atau NaN untuk float) |
| `a +? b`, `-?`, `*?` | `none` bila overflow |
| `a /? b`, `%?` | `none` bila `b == 0` atau `MIN / -1` |

## 6e. Generics dan trailing block (M5)
```
fn max[T](a, b T) T { if b > a { return b }; return a }   // T diinferensi dari argumen
fn map[T, U](xs []T, f fn(T) U) [..]U { ... }             // parameter fungsi

evens := xs.filter() { it % 2 == 0 }        // 'it' = parameter tunggal
total := xs.fold(0) { acc, x => acc +| x }
sort(&people) { a, b => a.age < b.age }
labels := xs.map() { x =>                   // block berisi beberapa statement
    if x % 3 == 0 { return 3 }
    return x % 3
}
twice(5, inc)                               // fungsi bernama juga bisa diteruskan
```
- **Generics** dimonomorfisasi. Setiap kombinasi tipe diperiksa ulang secara konkret, dengan semua aturan ketat. Misalnya `max(P{}, P{})` gagal di `>` untuk tipe `P`, dan error-nya menunjuk ke baris di template. Template yang tidak pernah dipanggil tidak diperiksa dan tidak menghasilkan kode. Generic struct belum ada.
- **Parameter fungsi `fn(A) R` tidak bisa escape.** Parameter ini hanya bisa dipanggil atau diteruskan ke parameter fungsi lain, tidak bisa disimpan, di-return, atau dimasukkan ke struct. Setiap pemanggilan dengan block menghasilkan spesialisasi tersendiri, sehingga LLVM meng-inline block menjadi loop langsung, tanpa closure di heap atau pemanggilan tidak langsung.
- **`return` di dalam block** mengembalikan nilai dari block. Ekspresi terakhir juga menjadi nilai block.
- **Analisis keamanan block:**
  - block bisa dipanggil berapa kali pun, jadi variabel pemanggil yang diubahnya dianalisis seperti badan loop (fixpoint);
  - block tidak boleh mengubah variabel yang juga diteruskan ke pemanggilan yang sama;
  - setelah pemanggilan, compiler memakai invarian hasil analisis itu.
- **Sintaks method untuk fungsi bebas:** `xs.filter() { ... }` sama dengan `filter(xs) { ... }`, berlaku untuk array.

**Prelude** (ditulis dalam tree-lang, ikut di setiap program, dan bisa diganti dengan mendefinisikan nama yang sama): `each`, `filter`, `map`, `fold`, `count`, `any`, `all`, `find` (→ `i64?`), `sort` (insertion sort, stabil), `min`, `max`.

## 6f. `return e from f` (M6)
```
fn load(lines []i64) ([..]User, i64) {
    var users [..]User
    for i, line := range lines { users.push(parse_user(line, i)) }
    return users, 0
}
fn parse_user(line i64, i i64) User {           // tanda tangan biasa: tanpa Result/error
    if line < 0 { return [..]User{}, line from load }
    return User{age: line, id: i}
}
fn first_big(xs []i64, limit i64) i64? {
    xs.each() { if it > limit { return it from first_big } }   // non-local return dari block
    return none
}
```
- **Semantik.** Nilai `e` menjadi hasil dari pemanggilan **aktif terdalam** dari `f`, sejauh apa pun letaknya di stack. Frame di antaranya tidak perlu menulis apa-apa, dan array tumbuh miliknya dibebaskan otomatis.
- **Dicek saat compile.** Untuk setiap fungsi dan block, compiler menghitung target `from` yang bisa terjadi lewat graf pemanggilan. Jika masih ada target di `main`, artinya `return ... from f` bisa terjadi tanpa `f` aktif, dan program ditolak dengan lokasi pemanggilannya. Targetnya harus fungsi bernama dan bukan generik.
- **Implementasi tanpa unwinder.**
  - `return e from f` menyimpan `e` di slot milik `f`, menandai id `f`, lalu keluar.
  - Setelah setiap pemanggilan yang *bisa* melompat, ada satu pengecekan tanda. Frame `f` menangkap nilainya, frame lain meneruskan ke atas.
  - Pemanggilan yang tidak bisa melompat tidak mendapat pengecekan apa pun, jadi tanpa biaya.

## 6g. Pool generasional, link relatif, serialisasi (M7)

**`Pool[T]`: kepemilikan bersama tanpa reference counting.**
```
var enemies Pool[Enemy]
h := enemies.add(Enemy{...})     // Handle[Enemy] = { indeks u32, generasi u32 }
enemies.get(h)                   // Enemy?  — none bila h sudah dihapus
enemies.set(h, e)  enemies.remove(h)  enemies.contains(h)  enemies.count()
```
Setiap slot menyimpan generasi. `remove` menaikkan generasi slot itu, dan slot dipakai ulang oleh `add` berikutnya. Handle lama tidak lagi cocok, sehingga hasilnya **`none`, bukan data milik pemilik baru dan bukan crash**. Di Goose, slot pool yang basi masih membaca data pemilik barunya.

**`Link[T]`: offset u32 bertipe dari awal array `[..]T`.**
```
type Node struct { value i64; left, right Link[Node]? }
l := nodes.push_link(Node{...})   // Link[Node]?
nodes.at(l)                       // Node?  (none bila di luar batas)
nodes.put(l, n)                   // bool
nodes.link(i)                     // Link[Node]? dari indeks
```
- Struktur tetap valid ketika array berpindah atau diserialisasi, karena link tidak memuat alamat.
- Setiap akses dicek terhadap panjang array saat itu, dan hasilnya `none` atau `false`, bukan panic.
- Sebuah struct boleh berisi `Link[` dirinya sendiri `]`.
- Berbeda dari Goose, offset diukur dari awal array, bukan dari field-nya.

**`to_bytes(x)` / `from_bytes(bytes, &out)`.**
- **Tipe yang didukung:** hanya tipe datar, yaitu angka, bool, struct, array tetap, enum, optional, `Handle`, dan `Link` ke tipe elemen yang sama.
- **Format:** `"TLB1"` | hash struktur tipe | jumlah elemen | field-field dikemas tanpa padding (little-endian). Padding memori tidak pernah ikut tersalin.
- **`from_bytes` memvalidasi semuanya sebelum menjadi nilai:**
  - magic dan hash tipe;
  - jumlah elemen;
  - bool harus 0/1;
  - tag enum dan optional harus sah;
  - setiap `Link[T]` harus kurang dari jumlah elemen;
  - tidak boleh ada sisa byte.

  Jika ada yang tidak sah, hasilnya `false` dan `out` **tidak diubah**.
- **Ukuran bisa berbeda per elemen.** Karena optional dan enum ditulis seperlunya, ukuran tiap elemen di data bisa berbeda. Misalnya `none` hanya 1 byte.

## 6h. Goroutine dan channel (M8, menggantikan thread fn)
```
fn worker(jobs Chan[Job], results Chan[Result]) {
    for job := range jobs {                     // berhenti saat jobs ditutup dan kosong
        ok := results <- Result{...}            // bool: false bila results tertutup
        if !ok { return }
    }
}
jobs := chan[Job](16)                           // kapasitas wajib (0 = tanpa buffer)
t := run worker(jobs, results)                  // goroutine baru → Task
_ = jobs <- Job{n: 1}
close(jobs)
if some r := <-results { ... }                  // <-ch : T?  (none bila tertutup dan kosong)
select {
    x := <-a { }      ok := b <- v { }      timeout(100) { }      default { }
}
ok := wait(t)                                   // false bila goroutine itu abort (atau sudah di-wait)
```
**Tidak ada data race, karena dicegah lewat konstruksi bahasa (bukan sekadar anjuran seperti di Go):**
- `run f(args)` hanya menerima fungsi bernama, dan argumennya disalin (data datar atau `Chan`). Tidak ada closure, sehingga goroutine tidak bisa menangkap variabel goroutine lain. Bahasa ini juga tidak punya variabel global yang bisa diubah.
- Isi channel berupa data datar yang disalin, atau nilai pemilik (`[..]T`, struct berisi `[..]T`) yang **dipindahkan**. Hanya region-nya yang berpindah, tanpa salinan. Setelah `ch <- xs`, memakai `xs` adalah error compile, juga bila hanya *mungkin* sudah dipindah di salah satu cabang. Assignment baru menghidupkannya lagi.
- Satu-satunya objek yang dibagi adalah channel dan task milik runtime, dan semuanya dikunci.

**Bug konkurensi lain dan penanganannya:**

| Masalah | Penanganan |
|---|---|
| Kirim ke channel tertutup (panic di Go) | `ch <- v` menghasilkan `false` (must-use). Nilai pemilik yang gagal terkirim dibebaskan. |
| Terima dari channel tertutup | `none`, dan `for x := range ch` berhenti |
| `close` dua kali (panic di Go) | no-op |
| Deadlock global | Runtime mendeteksi bila semua goroutine menunggu, lalu `abort: deadlock ...` (exit 101) |
| Buffer membengkak | Kapasitas channel wajib, sehingga pengirim menunggu bila penuh |
| Panic di goroutine (mematikan program di Go) | Hanya goroutine itu yang berhenti, region-nya dilepas, dan `wait` mengembalikan `false` |
| Stack overflow goroutine | Ukuran stack dari analisis stack compiler ditambah `#max_depth` |
| Urutan pesan antar goroutine | Tidak deterministik. `select` adil secara acak, jadi jangan bergantung pada urutan. |

**Implementasi:**
- Scheduler M:N: N worker (jumlah core, atur dengan `TL_WORKERS`). Setiap goroutine adalah fiber dengan stack sendiri, berukuran perkiraan stack compiler ditambah 96 KiB (rentang 128 KiB sampai 1 MiB).
- `main` sendiri juga goroutine.
- Goroutine hanya berpindah di titik tunggu (channel, `select`, `wait`, `sleep_ms`). `sleep_ms` dan `timeout` tidak memblokir worker.

**Batasan:**
- I/O file dan stdin memblokir satu worker.
- Satu run-queue global (belum work-stealing).
- `select` belum bisa mengirim atau menerima nilai pemilik.
- Channel yang tidak pernah ditutup baru dilepas saat program selesai.
- `a<-b` dibaca sebagai kirim channel. Tulis `a < -b` untuk perbandingan.
- Jalur POSIX (`ucontext`) belum diuji.

## 6i. FFI, stdlib, rekursi, tooling (M9)

**FFI: `unsafe extern fn`.** Fungsi C dideklarasikan tanpa badan:
```
unsafe extern fn hypot(x, y f64) f64       // libc/libm langsung tersedia
unsafe extern fn sum_bytes(s []u8) i64      // []T → (ptr, i64)
unsafe extern fn fill(out &i64, v i64)      // &T  → ptr
```
- Tipe parameter yang diizinkan: angka, bool (sebagai `int`), `[]T` dan `&T` dengan `T` datar. Hasil: angka, bool, atau tanpa nilai.
- File `<program>.c` di samping `<program>.tree` dikompilasi otomatis. File C lain bisa ditambahkan dengan `--c file.c`.
- Kata `unsafe` wajib. Tanpa `unsafe`, program ditolak.
- `treelang check --unsafe-report` mendaftar semua deklarasi dan pemanggilan FFI, yaitu semua tempat yang keamanannya bergantung pada kode C.

**Stdlib** (prelude: pembungkus aman di atas runtime; kegagalan selalu lewat `bool` / `T?`):

| Fungsi | |
|---|---|
| `read_file(path, &out) bool`, `write_file(path, data) bool`, `append_file`, `file_exists`, `delete_file` | file |
| `read_line(&out) bool`, `write_stderr(s)` | stdin/stderr |
| `arg_count() i64`, `arg(i, &out) bool`, `env(name, &out) bool` | argumen (`treelang run f.tree -- a b`) & lingkungan |
| `time_ns()`, `clock_ns()`, `sleep_ms(ms)`, `random_seed() u64` | waktu |
| `str(a, b, ...) [..]u8` | membangun string dengan format yang sama seperti `print` |
| `parse_int(s) i64?`, `parse_float(s) f64?` | parsing ketat (overflow → `none`) |
| `equal(a, b) bool`, `index_of(xs, x) i64?`, `contains(xs, x) bool` | generik untuk array |

**Rekursi dan stack native.**
- Fungsi yang rekursif, termasuk rekursi tidak langsung (`a → b → a`), wajib diberi `#max_depth(N)`.
- Saat runtime, kedalamannya dihitung per thread. Melampaui `N` menghasilkan `abort` dengan pesan yang jelas, bukan stack overflow.
- Compiler memperkirakan pemakaian stack native terburuk dari `main` dan dari setiap `thread fn` lewat graf pemanggilan. Perkiraannya konservatif: frame dihitung dua kali ukuran variabel, dan komponen rekursif dikalikan `N`. Program ditolak jika hasilnya melebihi 896 KiB.

**Tooling.**
- `treelang check --panic-report` mendaftar semua `abort(...)` eksplisit.
- `editors/vscode/` berisi syntax highlighting untuk file `.tree`.

## 6j. Package dan import

Model package-nya mengikuti Go. Satu **modul** adalah folder yang berisi `forest.json`, dan setiap folder di dalamnya adalah satu **package**.

```
myapp/
  forest.json            { "module": "myapp" }
  main.tree              package main
  config.tree            package main          (semua file di satu folder = satu package)
  geo/point.tree         package geo
  geo/shape.tree         package geo
  text/mathx/m.tree      package mathx         (import "myapp/text/mathx")
```
```
package main

import (
    "myapp/geo"
    m "myapp/text/mathx"          // alias
)

fn main() {
    p := geo.Point{X: 3, Y: 4}    // struct dari package lain
    print(geo.Dist2(p), p.Sum())  // fungsi & method
    var ps [..]geo.Point          // tipe berkualifikasi di mana pun tipe boleh ditulis
    c := geo.Color.Red            // varian enum: paket.Enum.Varian
    s := geo.Shape.Square{Side: 2}
    print(geo.Max, m.Square(3))   // konstanta
    t := run geo.Worker(ch)       // goroutine dari package lain
}
```

**`forest.json`.**
- Field `"module"` wajib, misalnya `"myapp"` atau `"github.com/me/app"`. Field lain diabaikan untuk saat ini (tempat `"dependencies"` nanti, saat package manager datang).
- JSON yang rusak ditolak dengan pesan berisi baris dan kolom.

**Menjalankan.**
- `treelang run myapp`, `treelang run .`, atau `treelang run myapp/main.tree`. Bila input berupa file, seluruh foldernya dimuat sebagai `package main`.
- `build` tanpa `-o` memberi nama executable sesuai nama folder.
- Tanpa `forest.json` di folder input atau di atasnya, file tunggal dijalankan seperti biasa (mode skrip). Klausa `package` boleh tidak ditulis, tetapi `import` tidak tersedia.

**Aturan:**
- Setiap file di modul diawali `package <nama>`. Nama package = nama foldernya. Folder program adalah `package main` dan memuat `fn main()`. `main` di package lain ditolak.
- Path import selalu ditulis dari root modul: `"<module>/<folder>/..."`. Path di luar modul menghasilkan "not found". Package eksternal akan tersedia lewat package manager.
- **Ekspor dengan huruf besar** (Go): `Add`, `Point`, `Max`, field `X`, dan method `Sum` terlihat dari luar package. Nama berhuruf kecil hanya terlihat di package itu, termasuk di file lain dalam folder yang sama.
  - Varian enum mengikuti enum-nya.
  - Literal struct dari package lain hanya boleh mengisi field yang diekspor. Jika ada field privat, buat lewat fungsi konstruktor, misalnya `geo.New(...)`.
- Method hanya bisa dideklarasikan untuk tipe dari package sendiri.
- **Ketat**:
  - import yang tidak dipakai = error;
  - import siklik = error, beserta rantainya (`a -> b -> a`);
  - nama yang sama diimpor dua kali = error;
  - nama duplikat antar file dalam satu package = error;
  - variabel atau parameter yang menimpa nama import = error. Dengan begitu `geo.X` tidak pernah ambigu dengan akses field.
- Prelude (`filter`, `map`, `str`, `read_file`, ...) terlihat di semua package tanpa kualifikasi. Fungsi generik dari package lain diinstansiasi dengan tipe milik pemanggil, dan badannya tetap melihat nama privat package asalnya.
- Sidecar `<file>.c` di samping setiap file `.tree` di semua package ikut dikompilasi.

**Implementasi.**
- Deklarasi package non-main diberi nama internal `"<importpath>.Nama"` (simbol LLVM `@"tl.f.myapp/geo.Dist2"`). Dua package boleh memiliki nama yang sama tanpa bentrok.
- Pesan error dan `print` menampilkannya sebagai `geo.Dist2` / `geo.Point { X: 1, Y: 2 }`.
- Setiap error menunjuk file asalnya di dalam package.

## 7. Pembuktian keamanan
Compiler melacak rentang setiap integer: variabel **dan** field (`p.x`, `a.b.c`).
- Fakta diambil dari literal, konstanta, nilai nol, literal struct, kondisi `if`/`for`/`&&`/`||`, dan rentang `for ... range`.
- Loop: invarian dicari dengan fixpoint + widening berambang. Akumulator di `for ... range` dibatasi oleh analisis induksi.
- Jika tidak terbukti, hasilnya error dengan saran: tambahkan pemeriksaan `if`, perlebar tipe, atau pilih operator wrap/saturasi.

Aturan lain:
- hasil fungsi tidak boleh diabaikan (pakai `_ =`);
- fungsi bernilai harus `return` di semua jalur;
- konstanta tidak bisa diubah;
- struct tidak boleh memuat dirinya sendiri.

## 8. Runtime
- `print(a, b, ...)` mencetak argumen berurutan lalu newline. Struct dicetak sebagai `Point { x: 1, y: 2 }`.
- `abort("pesan")` adalah satu-satunya cara program berhenti dengan error (exit code 101).
- `--debug` memverifikasi ulang semua bukti saat runtime. Jika ada bukti yang salah (bug compiler), program keluar dengan exit code 102.

## 9. Kompilasi
`.tree` → lexer (penyisipan `;`) → parser → sema (tipe + bukti) → LLVM IR → `clang -O2` + `runtime/tl_runtime.c` → executable.
File sementara ditulis ke `TREELANG_TMP`, atau ke folder `tmp` di samping `treelang.exe`.
