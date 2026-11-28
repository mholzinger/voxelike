#!/usr/bin/env python3
"""The voxel model editor: a page in the browser for the art in models/.

usage: tools/voxedit.py [--port 8732] [--no-open] [--models DIR]      (or: make edit)

Serves tools/voxedit.html and the four files of models/ to this machine
only, and opens the page. The page reads the files as they are, so what it
shows is what tools/bake_models.py would bake, and saves a model by having
its block of its file written again: the rest of the file, comments and
all, is left as it is, and a file the bake would not take is not written.
--models DIR is another checkout's art. Needs nothing but Python.
"""
import argparse, http.server, json, os, re, sys, tempfile, threading, webbrowser

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bake_models

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGE = os.path.join(ROOT, "tools/voxedit.html")
FILES = ("materials.txt", "creatures.txt", "gear.txt", "terrain.txt")
MODELS = os.path.join(ROOT, "models")


class Handler(http.server.BaseHTTPRequestHandler):
    def reply(self, code, kind, body):
        self.send_response(code)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path in ("/", "/index.html"):
            return self.reply(200, "text/html; charset=utf-8", open(PAGE, "rb").read())
        # Only the four files, by name: nothing else on the disk is served.
        if path.startswith("/models/") and path[8:] in FILES:
            return self.reply(200, "text/plain; charset=utf-8",
                              open(os.path.join(MODELS, path[8:]), "rb").read())
        self.reply(404, "text/plain; charset=utf-8", b"not found\n")

    def do_POST(self):
        # Only this page may save: a page from anywhere else that knew the
        # port could otherwise write to these files.
        origin = self.headers.get("Origin")
        host = self.headers.get("Host", "")
        if self.path != "/save" or (origin and origin not in (f"http://{host}",)) or \
           host.split(":")[0] not in ("127.0.0.1", "localhost"):
            return self.reply(403, "text/plain; charset=utf-8", b"not from this page\n")
        try:
            body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))))
            save(body["file"], body["kind"], body["text"])
        except Refused as e:
            return self.reply(400, "text/plain; charset=utf-8", str(e).encode())
        except (KeyError, ValueError, TypeError) as e:
            return self.reply(400, "text/plain; charset=utf-8", f"bad request: {e}".encode())
        self.reply(200, "text/plain; charset=utf-8", b"saved\n")

    def log_message(self, *args):
        pass


class Refused(Exception):
    pass


def save(name, kind, text):
    """Write a model's block into its file in place of the one it has."""
    if name not in FILES[1:] or (kind == "top") != (name == "terrain.txt"):
        raise Refused(f"{name} is not a file of models")
    block = text.rstrip("\n").split("\n")
    head, body = block[0], block[1:]
    if not re.fullmatch(r"= \S.*", head) or not body or any(not re.fullmatch(r"\S+( \S+)*", line) for line in body):
        raise Refused("that is not a model's block")
    path = os.path.join(MODELS, name)
    lines = open(path).read().split("\n")
    # The block is from its heading to the next: of that, its layers are
    # the lines that are neither blank nor comment, and what comes after
    # the last of them belongs to the model below.
    want = head.split()
    at = [i for i, line in enumerate(lines) if line.startswith("=") and line.split() == want]
    if len(at) != 1:
        raise Refused(f"{name} has {len(at)} models headed '{head}': it has changed since the page read it")
    end = next((i for i in range(at[0] + 1, len(lines)) if lines[i].startswith("=")), len(lines))
    layers = [i for i in range(at[0] + 1, end) if lines[i].strip() and not lines[i].startswith("#")]
    first, last = (layers[0], layers[-1] + 1) if layers else (at[0] + 1, at[0] + 1)
    lines[first:last] = body

    # Nothing is written that tools/bake_models.py would not take.
    handle, trial = tempfile.mkstemp(dir=MODELS, prefix="." + name + ".")
    try:
        with os.fdopen(handle, "w") as f:
            f.write("\n".join(lines))
        mats = bake_models.read_materials(os.path.join(MODELS, "materials.txt"))
        keys = {key: i for i, (_, _, key) in enumerate(mats, 1) if key != "-"}
        try:
            if kind == "top":
                bake_models.read_terrain(trial, keys)
            else:
                bake_models.read_creatures(trial, keys)
        except SystemExit as e:
            raise Refused(str(e).replace(trial, "models/" + name))
        os.chmod(trial, os.stat(path).st_mode & 0o777)
        os.replace(trial, path)
    finally:
        if os.path.exists(trial):
            os.remove(trial)


def main():
    global MODELS
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8732)
    ap.add_argument("--no-open", action="store_true")
    ap.add_argument("--models", default=MODELS)
    args = ap.parse_args()
    MODELS = os.path.abspath(args.models)
    missing = [f for f in FILES if not os.path.exists(os.path.join(MODELS, f))]
    if missing:
        sys.exit(f"voxedit: {MODELS} has no {', '.join(missing)}")
    try:
        server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    except OSError as e:
        sys.exit(f"voxedit: port {args.port}: {e.strerror}. Is it already running? Try --port {args.port + 1}")
    url = f"http://127.0.0.1:{args.port}/"
    print(f"voxedit: {url}  (models in {MODELS}; ctrl-C to stop)")
    if not args.no_open:
        threading.Timer(0.3, webbrowser.open, [url]).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print()


if __name__ == "__main__":
    main()
