#!/usr/bin/env python3
"""Local pixel editor for the X11 BDF strikes under x11/.

Serves bdf-editor.html on 127.0.0.1 and patches single glyph blocks in place,
leaving every other byte of the BDF untouched. A save is refused if the glyph
changed on disk since it was loaded.

Usage: python3 lib/EpdFont/scripts/bdf-editor.py [--port 8765] [--no-browser]
"""

import argparse
import hashlib
import json
import os
import tempfile
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[2]
X11_DIR = REPO_ROOT / "x11"
HTML_PATH = SCRIPT_DIR / "bdf-editor.html"


def resolve_bdf(rel):
    path = (REPO_ROOT / rel).resolve()
    if X11_DIR.resolve() not in path.parents or path.suffix != ".bdf" or not path.is_file():
        raise ValueError(f"not a BDF under x11/: {rel}")
    return path


def block_hash(lines):
    return hashlib.sha1("\n".join(lines).encode()).hexdigest()


def parse_bdf(text):
    lines = text.split("\n")
    props, fbb, glyphs = {}, [0, 0, 0, 0], []
    in_props = False
    seen = {}
    i = 0
    while i < len(lines):
        line = lines[i]
        word = line.split(" ", 1)[0]
        if word == "FONTBOUNDINGBOX":
            fbb = [int(v) for v in line.split()[1:5]]
        elif word == "STARTPROPERTIES":
            in_props = True
        elif word == "ENDPROPERTIES":
            in_props = False
        elif in_props:
            key, _, value = line.partition(" ")
            value = value.strip()
            props[key] = value[1:-1] if value.startswith('"') else value
        elif word == "STARTCHAR":
            start = i
            while lines[i] != "ENDCHAR":
                i += 1
            block = lines[start : i + 1]
            glyph = parse_glyph(block)
            # ENCODING -1 is not unique; key those by name and occurrence.
            base = str(glyph["enc"]) if glyph["enc"] >= 0 else f"-1:{glyph['name']}"
            seen[base] = seen.get(base, 0) + 1
            glyph["key"] = base if seen[base] == 1 else f"{base}#{seen[base]}"
            glyph["start"], glyph["end"] = start, i
            glyph["hash"] = block_hash(block)
            glyphs.append(glyph)
        i += 1
    return lines, props, fbb, glyphs


def parse_glyph(block):
    glyph = {"name": block[0][len("STARTCHAR ") :], "enc": -1, "dwidth": 0, "bbx": [0, 0, 0, 0], "hex": []}
    in_bitmap = False
    for line in block[1:-1]:
        parts = line.split()
        if in_bitmap:
            glyph["hex"].append(line.strip())
        elif not parts:
            continue
        elif parts[0] == "ENCODING":
            glyph["enc"] = int(parts[1])
        elif parts[0] == "DWIDTH":
            glyph["dwidth"] = int(parts[1])
        elif parts[0] == "BBX":
            glyph["bbx"] = [int(v) for v in parts[1:5]]
        elif parts[0] == "BITMAP":
            in_bitmap = True
    return glyph


def load_font(rel):
    path = resolve_bdf(rel)
    _, props, fbb, glyphs = parse_bdf(path.read_text(encoding="latin-1"))
    for g in glyphs:
        del g["start"], g["end"]
    return {"path": rel, "props": props, "fbb": fbb, "glyphs": glyphs}


def save_glyph(req):
    path = resolve_bdf(req["path"])
    text = path.read_text(encoding="latin-1")
    lines, props, _, glyphs = parse_bdf(text)
    matches = [g for g in glyphs if g["key"] == req["key"]]
    if len(matches) != 1:
        return 404, {"error": f"glyph {req['key']} not found"}
    old = matches[0]
    if old["hash"] != req["hash"]:
        del old["start"], old["end"]
        return 409, {"error": "glyph changed on disk since it was loaded", "glyph": old}

    w, h, x, y = (int(v) for v in req["bbx"])
    dwidth = int(req["dwidth"])
    hex_rows = [str(r).upper() for r in req["hex"]]
    row_chars = ((w + 7) // 8) * 2
    if len(hex_rows) != h or any(len(r) != row_chars or any(c not in "0123456789ABCDEF" for c in r) for r in hex_rows):
        return 400, {"error": "bitmap does not match BBX"}

    block = lines[old["start"] : old["end"] + 1]
    new_block, in_bitmap = [], False
    for line in block:
        word = line.split(" ", 1)[0]
        if in_bitmap and line != "ENDCHAR":
            continue
        if word == "SWIDTH" and dwidth != old["dwidth"]:
            # Scalable width in 1/1000 em from the device width at this strike's size.
            point = float(props.get("POINT_SIZE", "0")) / 10
            res_x = float(props.get("RESOLUTION_X", "0"))
            if point and res_x:
                line = f"SWIDTH {round(dwidth * 72000 / (point * res_x))} 0"
        elif word == "DWIDTH":
            line = f"DWIDTH {dwidth} 0"
        elif word == "BBX":
            line = f"BBX {w} {h} {x} {y}"
        elif word == "BITMAP":
            new_block.append(line)
            new_block.extend(hex_rows)
            in_bitmap = True
            continue
        new_block.append(line)

    lines[old["start"] : old["end"] + 1] = new_block
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=".bdf-editor-")
    with os.fdopen(fd, "w", encoding="latin-1", newline="") as f:
        f.write("\n".join(lines))
    os.chmod(tmp, path.stat().st_mode & 0o777)
    os.replace(tmp, path)

    saved = parse_glyph(new_block)
    saved["key"], saved["hash"] = req["key"], block_hash(new_block)
    return 200, {"glyph": saved}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def send_json(self, status, body):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        url = urlparse(self.path)
        try:
            if url.path == "/":
                data = HTML_PATH.read_bytes()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
            elif url.path == "/api/files":
                files = sorted(str(p.relative_to(REPO_ROOT)) for p in X11_DIR.glob("*/*.bdf"))
                self.send_json(200, {"files": files})
            elif url.path == "/api/font":
                self.send_json(200, load_font(parse_qs(url.query)["path"][0]))
            else:
                self.send_json(404, {"error": "not found"})
        except (ValueError, KeyError) as e:
            self.send_json(400, {"error": str(e)})

    def do_POST(self):
        if urlparse(self.path).path != "/api/glyph":
            return self.send_json(404, {"error": "not found"})
        try:
            req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            status, body = save_glyph(req)
            if status == 200:
                print(f"saved {req['path']} glyph {req['key']}")
            self.send_json(status, body)
        except (ValueError, KeyError) as e:
            self.send_json(400, {"error": str(e)})


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--no-browser", action="store_true")
    args = parser.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    url = f"http://127.0.0.1:{args.port}/"
    print(f"BDF editor at {url} (Ctrl-C to stop)")
    if not args.no_browser:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
