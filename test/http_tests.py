"""Test std/http: build test/http/app.tree, jalankan, lalu kirim request HTTP mentah lewat socket.

Pemakaian: python test/http_tests.py <treelang.exe> [--quick]
Setiap mode (-O2 dan --debug) menjalankan semua kasus; --quick hanya -O2 tanpa uji beban besar.
"""
import os
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
HTTP = ROOT / "http"
TMP = ROOT.parent / "build" / "tmp"


class Resp:
    def __init__(self, status, headers, body):
        self.status, self.headers, self.body = status, headers, body

    def h(self, name):
        return self.headers.get(name.lower())


def read_response(f, head=False):
    line = f.readline()
    if not line:
        return None
    parts = line.decode("latin-1").rstrip("\r\n").split(" ", 2)
    status = int(parts[1])
    headers = {}
    while True:
        l = f.readline().decode("latin-1").rstrip("\r\n")
        if not l:
            break
        k, v = l.split(":", 1)
        headers[k.strip().lower()] = v.strip()
    body = b""
    n = int(headers.get("content-length", "0"))
    if n and not head:
        body = f.read(n)
    return Resp(status, headers, body)


class Conn:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.f = self.s.makefile("rb")

    def send(self, data):
        self.s.sendall(data if isinstance(data, bytes) else data.encode("latin-1"))

    def req(self, method, path, headers=None, body=b"", head=False):
        h = {"Host": "x"}
        h.update(headers or {})
        if body:
            h["Content-Length"] = str(len(body))
        msg = f"{method} {path} HTTP/1.1\r\n" + "".join(f"{k}: {v}\r\n" for k, v in h.items()) + "\r\n"
        self.send(msg.encode("latin-1") + body)
        return read_response(self.f, head or method == "HEAD")

    def read(self, head=False):
        return read_response(self.f, head)

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def get(port, path, **kw):
    c = Conn(port)
    try:
        return c.req("GET", path, **kw)
    finally:
        c.close()


failures = []


def check(name, cond, detail=""):
    if not cond:
        failures.append(f"{name}: {detail}")


def suite(port, quick):
    r = get(port, "/")
    check("hello", r.status == 200 and r.body == b"Hello, World!", vars(r))
    check("server headers", r.h("server") == "tree-lang" and r.h("date") and r.h("x-served-by") == "tree-lang", r.headers)
    r = get(port, "/users/42?name=budi%20s&x=1")
    check("param+query", r.body == b"user 42 budi s", r.body)
    r = get(port, "/users/abc")
    check("param not int", r.body == b"user -1 ", r.body)
    r = get(port, "/users/42/")
    check("trailing slash", r.status == 200 and r.body == b"user 42 ", r.body)
    r = get(port, "/opt")
    check("optional missing", r.body == b"anonymous", r.body)
    r = get(port, "/opt/ani")
    check("optional given", r.body == b"hi ani", r.body)
    r = get(port, "/files/a/b/c.txt")
    check("wildcard", r.body == b"file:a/b/c.txt", r.body)
    r = get(port, "/nope")
    check("404", r.status == 404 and r.h("x-served-by") == "tree-lang", (r.status, r.headers))
    c = Conn(port)
    r = c.req("POST", "/", body=b"x")
    check("405", r.status == 405 and r.h("allow") == "GET, HEAD", (r.status, r.headers))
    r = c.req("HEAD", "/")
    check("HEAD", r.status == 200 and r.h("content-length") == "13" and r.body == b"", vars(r))
    r = c.req("POST", "/users", headers={"Content-Type": "text/plain"}, body=b"hello")
    check("post body", r.status == 201 and r.body == b'{"got":5,"body":"hello"}' and r.h("content-type") == "application/json",
          vars(r))
    r = c.req("DELETE", "/any")
    check("All", r.body == b"DELETE", r.body)
    r = c.req("GET", "/headers", headers={"X-Test": "abc 123"})
    check("request header", r.h("x-got") == "abc 123", r.headers)
    r = c.req("GET", "/old")
    check("redirect", r.status == 301 and r.h("location") == "/", (r.status, r.headers))
    r = c.req("GET", "/api/secret")
    check("middleware stop", r.status == 401 and r.body == b"unauthorized", vars(r))
    r = c.req("GET", "/api/secret", headers={"X-Token": "secret"})
    check("middleware pass", r.status == 200 and r.body == b"secret data", vars(r))
    r = c.req("GET", "/api/v1/secret")
    check("nested group inherits middleware", r.status == 401, r.status)
    r = c.req("GET", "/crash")
    check("panic → 500", r.status == 500 and r.body == b"Internal Server Error", vars(r))
    r = c.req("GET", "/")
    check("alive after panic (same conn)", r.status == 200, r.status)
    r = c.req("GET", "/big")
    check("big body", r.status == 200 and len(r.body) == 100000, len(r.body))
    c.close()

    # chunked request body
    c = Conn(port)
    c.send("POST /users HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n4;ext=1\r\ndefg\r\n0\r\n\r\n")
    r = c.read()
    check("chunked", r and r.body == b'{"got":7,"body":"abcdefg"}', r and r.body)
    c.close()

    # pipelining: 10 request dalam satu kiriman, urutan respons terjaga
    c = Conn(port)
    c.send("".join(f"GET /users/{i} HTTP/1.1\r\nHost: x\r\n\r\n" for i in range(10)))
    got = [c.read().body for _ in range(10)]
    check("pipelining", got == [f"user {i} ".encode() for i in range(10)], got)
    c.close()

    # HTTP/1.0 tanpa keep-alive → koneksi ditutup
    c = Conn(port)
    c.send("GET / HTTP/1.0\r\n\r\n")
    r = c.read()
    rest = c.f.read()
    check("http/1.0 close", r.status == 200 and r.h("connection") == "close" and rest == b"", (r.headers, rest))
    c.close()

    # error protokol
    for name, raw, code in [
        ("bad request line", "BLAH\r\n\r\n", 400),
        ("bad version", "GET / HTTP/9.9\r\n\r\n", 505),
        ("unknown method", "BREW /coffee HTTP/1.1\r\nHost: x\r\n\r\n", 501),
        ("too large body", "POST /users HTTP/1.1\r\nContent-Length: 5000\r\n\r\n", 413),
        ("header too large", "GET / HTTP/1.1\r\nX-Big: " + "a" * 9000 + "\r\n\r\n", 431),
        ("cl + te", "POST /users HTTP/1.1\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n", 400),
        ("bad percent", "GET /users/%zz HTTP/1.1\r\n\r\n", 400),
    ]:
        c = Conn(port)
        c.send(raw)
        r = c.read()
        check(name, r is not None and r.status == code, r and r.status)
        c.close()

    # file statis
    r = get(port, "/static/a.txt")
    check("static file", r.status == 200 and r.body == b"hello static\n" and r.h("content-type").startswith("text/plain"),
          vars(r))
    r2 = get(port, "/static/a.txt", headers={"If-Modified-Since": r.h("last-modified")})
    check("static 304", r2.status == 304 and r2.body == b"", r2.status)
    r = get(port, "/static/")
    check("static index", r.status == 200 and b"index" in r.body, r.body)
    r = get(port, "/static/sub/s.css")
    check("static css", r.h("content-type", ) and "text/css" in r.h("content-type"), r.headers)
    r = get(port, "/static/../app.tree")
    check("static traversal", r.status in (403, 404) and b"import" not in r.body, (r.status, r.body[:40]))

    # worker mati (abort): koneksi ditutup setelah HandlerTimeout, server tetap hidup
    c = Conn(port)
    c.send("GET /die HTTP/1.1\r\nHost: x\r\n\r\n")
    t0 = time.time()
    try:
        data = c.f.read()
    except OSError:
        data = b""
    check("abort closes conn", data == b"" and time.time() - t0 < 8, data)
    c.close()
    r = get(port, "/")
    check("alive after abort", r.status == 200, r.status)

    # beban: banyak koneksi keep-alive bersamaan
    conns, per = (20, 300) if quick else (50, 2000)
    errors = []

    def load():
        try:
            cc = Conn(port)
            for i in range(per):
                rr = cc.req("GET", f"/users/{i}")
                if rr.status != 200 or rr.body != f"user {i} ".encode():
                    errors.append(rr.status)
                    break
            cc.close()
        except Exception as e:  # noqa
            errors.append(repr(e))

    t0 = time.time()
    ts = [threading.Thread(target=load) for _ in range(conns)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    dt = time.time() - t0
    check("load", not errors, errors[:5])
    return conns * per, dt


def run_mode(exe, mode, quick):
    TMP.mkdir(parents=True, exist_ok=True)
    out = TMP / f"http_app{mode.replace('-', '_')}.exe"
    b = subprocess.run([exe, "build", str(HTTP / "app.tree"), "-o", str(out), mode], capture_output=True, text=True)
    if b.returncode != 0:
        failures.append(f"build {mode}: {b.stderr}")
        return
    p = subprocess.Popen([str(out)], cwd=HTTP, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        line = p.stdout.readline().decode()
        port = int(line.split("(port ")[1].split(")")[0])
        n, dt = suite(port, quick)
        print(f"  {mode}: {n} requests under load in {dt:.2f}s ({n / dt:.0f} req/s, Python client)")
    finally:
        p.kill()
        p.wait()
        for ext in (".exe", ".pdb", ".ilk"):
            try:
                os.remove(out.with_suffix(ext))
            except OSError:
                pass


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    exe = os.path.abspath(sys.argv[1])
    quick = "--quick" in sys.argv
    for mode in (["-O2"] if quick else ["-O2", "--debug"]):
        before = len(failures)
        run_mode(exe, mode, quick)
        print(f"{'PASS' if len(failures) == before else 'FAIL'}  http {mode}")
    for f in failures:
        print("  " + f)
    print(f"\nhttp: {'all passed' if not failures else str(len(failures)) + ' failure(s)'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
