"""Test runner tree-lang.

  test/ok/*.tree   harus lolos compile dan mencetak tepat baris-baris `// expect: ...`.
                   Setiap test dijalankan 3 kali: -O2, -O0, dan --debug (semua bukti
                   compile-time diverifikasi ulang saat runtime). Output harus identik.
                   `// env: K=V` mengatur variabel lingkungan; `// stderr: teks` wajib muncul di stderr.
  test/err/*.tree  harus DITOLAK compiler (exit 1) dan stderr memuat setiap `// error: ...`.
  test/pkg/ok/<modul>/, test/pkg/err/<modul>/
                   program multi-package (folder dengan forest.json); direktif ditulis di main.tree
                   dan compiler dipanggil dengan folder itu sebagai input.

Pemakaian: python test/run_tests.py <path ke forest.exe> [filter]
"""
import concurrent.futures
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
MODES = [["-O2"], ["-O0"], ["--debug"]]


def directives(path, key):
    if path.is_dir():
        path = path / "main.tree"
    out = []
    for line in path.read_text(encoding="utf-8").splitlines():
        s = line.strip()
        tag = "// " + key + ":"
        if s.startswith(tag):
            v = s[len(tag):]
            out.append(v[1:] if v.startswith(" ") else v)
    return out


def run(cmd, env=None):
    full = None
    if env:
        full = dict(os.environ)
        full.update(env)
    p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", env=full)
    return p.returncode, p.stdout, p.stderr


def check_ok(exe, path):
    want = directives(path, "expect")
    want_exit = directives(path, "exit")
    want_code = int(want_exit[0]) if want_exit else 0
    env = dict(kv.split("=", 1) for kv in directives(path, "env"))  # // env: K=V (untuk program)
    want_err = directives(path, "stderr")
    for mode in MODES:
        code, out, err = run([exe, "run", str(path)] + mode, env)
        missing = [w for w in want_err if w not in err]
        if missing:
            return f"{' '.join(mode)}: missing on stderr: {missing}\n{err}"
        got = out.splitlines()
        if code != want_code:
            return f"{' '.join(mode)}: exit code {code} (want {want_code})\n{err}"
        if got != want:
            diff = []
            for i in range(max(len(got), len(want))):
                g = got[i] if i < len(got) else "<missing>"
                w = want[i] if i < len(want) else "<missing>"
                if g != w:
                    diff.append(f"  line {i + 1}: got {g!r}, want {w!r}")
            return f"{' '.join(mode)}: output mismatch\n" + "\n".join(diff[:10])
    return None


def check_err(exe, path):
    want = directives(path, "error")
    if not want:
        return "no '// error:' directive in test"
    code, out, err = run([exe, "check", str(path)])
    if code != 1:
        return f"expected compile failure (exit 1), got exit {code}\n{err}"
    missing = [w for w in want if w not in err]
    if missing:
        return "missing expected error(s):\n" + "\n".join("  " + m for m in missing) + "\n--- stderr ---\n" + err
    return None


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    exe = os.path.abspath(sys.argv[1])
    filt = sys.argv[2] if len(sys.argv) > 2 else ""
    jobs = []
    for kind, fn in (("ok", check_ok), ("err", check_err)):
        for p in sorted((ROOT / kind).glob("*.tree")):
            if filt in p.name:
                jobs.append((f"{kind}/{p.name}", fn, p))
        for p in sorted((ROOT / "pkg" / kind).glob("*/")):
            if p.is_dir() and filt in p.name:
                jobs.append((f"pkg/{kind}/{p.name}", fn, p))
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        futs = {pool.submit(fn, exe, p): name for name, fn, p in jobs}
        results = {futs[f]: f.result() for f in concurrent.futures.as_completed(futs)}
    for name, _, _ in jobs:
        r = results[name]
        if r is None:
            print(f"PASS  {name}")
        else:
            failed += 1
            print(f"FAIL  {name}\n{r}")
    print(f"\n{len(jobs) - failed}/{len(jobs)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
