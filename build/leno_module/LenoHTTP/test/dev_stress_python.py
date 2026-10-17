# dev_stress_python.py —— 用 Python 标准库当**独立 HTTP 客户端**压 LenoHTTP（开发期工具）
#
# 为什么需要：Leno 侧的 test_http 是"自己写的客户端验证自己写的服务端"，容易两边一起错。
# 这里用 CPython 的 http.client / socket 当尺子 —— 它是一个完全独立的 HTTP 实现：
#   · 顺序 100 个请求（每次新连接）
#   · 单连接 keep-alive 20 个请求
#   · POST 表单（中文 urlencoded）与 4KB JSON
#   · 404 / 405 / 畸形请求 400
#   · 静态文件（200KB）sha256 逐字节校验
#   · 10 线程 × 10 请求并发（服务端单线程 ⇒ 会排队，但不许出错/挂死）
#
# 用法（仓库根目录）：
#     python build/leno_module/LenoHTTP/test/dev_stress_python.py
import hashlib
import http.client
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
EXE = os.path.join(ROOT, "build", "leno.exe")
SERVER = os.path.join(HERE, "..", "examples", "hello_server.leno")

fails = 0


def check(cond, tag, detail=""):
    global fails
    if not cond:
        fails += 1
        print(f"[FAIL] {tag} {detail}")


def pick_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def wait_port(port, timeout=15.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.3):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def get(port, path, timeout=10):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    c.request("GET", path)
    r = c.getresponse()
    body = r.read()
    c.close()
    return r.status, body, dict(r.getheaders())


def main():
    port = pick_port()
    static_dir = tempfile.mkdtemp(prefix="lenohttp_www_")
    with open(os.path.join(static_dir, "hello.txt"), "w", encoding="utf-8") as f:
        f.write("static-ok")
    big = bytes((65 + (i % 26)) for i in range(200000))
    with open(os.path.join(static_dir, "big.bin"), "wb") as f:
        f.write(big)
    big_sha = hashlib.sha256(big).hexdigest()

    proc = subprocess.Popen(
        [EXE, SERVER, str(port), static_dir],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    try:
        if not wait_port(port):
            out = ""
            try:
                proc.kill()
                out = proc.stdout.read().decode("utf-8", "replace")[:800]
            except Exception:
                pass
            print("[FAIL] 服务没起来（15s 内连不上）")
            print(out)
            return 1

        # ---------- ① 顺序 100 个请求（每次新连接）----------
        t0 = time.time()
        for i in range(100):
            st, body, _ = get(port, "/")
            check(st == 200 and body == b"hello", "① 顺序请求", f"#{i} status={st} body={body[:20]!r}")
        dt = time.time() - t0
        print(f"① 顺序 100 请求：{dt*1000/100:.1f} ms/请求（{100/dt:.0f} req/s）")

        # ---------- ② keep-alive：一个连接 20 个请求 ----------
        c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        ok2 = True
        for i in range(20):
            c.request("GET", "/echo?q=keep")
            r = c.getresponse()
            b = r.read()
            if r.status != 200 or json.loads(b).get("q") != "keep":
                ok2 = False
                check(False, "② keep-alive", f"#{i} status={r.status} body={b[:60]!r}")
                break
        c.close()
        if ok2:
            print("② keep-alive 20 个请求 ok")

        # ---------- ③ POST 表单（中文）----------
        form = urllib.parse.urlencode({"name": "张三", "age": "30"}).encode()
        c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        c.request("POST", "/form", body=form,
                  headers={"Content-Type": "application/x-www-form-urlencoded"})
        r = c.getresponse()
        data = json.loads(r.read())
        c.close()
        check(r.status == 200 and data.get("name") == "张三" and data.get("age") == "30",
              "③ 表单回显", f"status={r.status} data={data}")

        # ---------- ④ POST JSON 4KB ----------
        payload = json.dumps({"k": "v" * 4000}).encode()
        c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        c.request("POST", "/json", body=payload, headers={"Content-Type": "application/json"})
        r = c.getresponse()
        got = json.loads(r.read())
        c.close()
        check(r.status == 200 and got.get("bytes") == len(payload), "④ JSON 请求体", f"{got}")

        # ---------- ⑤ 404 / 405 / 畸形请求 400 ----------
        st, _, _ = get(port, "/missing")
        check(st == 404, "⑤ 404", f"status={st}")

        c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        c.request("DELETE", "/")
        r = c.getresponse()
        r.read()
        c.close()
        check(r.status == 405, "⑤ 405", f"status={r.status}")

        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        s.sendall(b"BROKEN\r\n\r\n")
        resp = s.recv(4096)
        s.close()
        check(resp.startswith(b"HTTP/1.1 400"), "⑤ 畸形请求 400", f"resp={resp[:40]!r}")

        # ---------- ⑥ 静态文件（含 200KB 大响应逐字节）----------
        st, body, hdrs = get(port, "/assets/hello.txt")
        check(st == 200 and body == b"static-ok", "⑥ 小静态文件", f"status={st}")
        check("text/plain" in hdrs.get("Content-Type", ""), "⑥ Content-Type", hdrs.get("Content-Type"))

        st, body, _ = get(port, "/assets/big.bin", timeout=20)
        check(st == 200 and len(body) == 200000, "⑥ 大静态文件长度", f"status={st} len={len(body)}")
        check(hashlib.sha256(body).hexdigest() == big_sha, "⑥ 大静态文件内容（sha256）")

        # ---------- ⑦ 并发：10 线程 × 10 请求 ----------
        errors = []
        counter = {"n": 0}
        lock = threading.Lock()

        def worker(tid):
            for i in range(10):
                try:
                    st, b, _ = get(port, "/")
                    if st != 200 or b != b"hello":
                        with lock:
                            errors.append(f"t{tid}#{i} status={st}")
                except Exception as e:
                    with lock:
                        errors.append(f"t{tid}#{i} {e}")

        t0 = time.time()
        ths = [threading.Thread(target=worker, args=(i,)) for i in range(10)]
        for t in ths:
            t.start()
        for t in ths:
            t.join(60)
        dt = time.time() - t0
        check(len(errors) == 0, "⑦ 并发 100 请求", f"{len(errors)} 个错误：{errors[:3]}")
        print(f"⑦ 并发 10×10：{dt*1000:.0f} ms 总计（{100/dt:.0f} req/s）")

        print("FAILS =", fails)
        return 1 if fails else 0
    finally:
        try:
            proc.kill()
        except Exception:
            pass
        try:
            remaining = proc.stdout.read().decode("utf-8", "replace")
            if remaining.strip():
                print("--- 服务端输出（尾部 600 字）---")
                print(remaining[-600:])
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())
