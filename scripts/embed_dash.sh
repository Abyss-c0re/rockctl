#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-$ROOT/../clanker-dash/www/index.html}"
[[ -f "$SRC" ]] || SRC="$ROOT/www/index.html"
cp -a "$SRC" "$ROOT/www/index.html"
python3 - "$ROOT/www/index.html" "$ROOT/src/clanker_dash_html.h" <<'PY'
import sys
from pathlib import Path
src = Path(sys.argv[1]).read_text(encoding="utf-8")
out_path = Path(sys.argv[2])
lines = src.splitlines(keepends=True)
parts = ["#ifndef CLANKER_DASH_HTML_H\n", "#define CLANKER_DASH_HTML_H\n",
         "static const char CLANKER_DASH_HTML[] =\n"]
for line in lines:
    body = line.replace("\\", "\\\\").replace('"', '\\"').replace("\r", "")
    if body.endswith("\n"):
        body = body[:-1]
        parts.append(f'  "{body}\\n"\n')
    else:
        parts.append(f'  "{body}"\n')
parts.append(";\n#endif\n")
out_path.write_text("".join(parts))
print(f"embedded {len(src)} bytes → {out_path}")
PY
