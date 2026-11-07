#!/usr/bin/env python3
"""Set one field of [[program]] entries in a manifest, keeping the rest of the file as is.

    tools/corpus/setfield.py MANIFEST KEY VALUE FILE...

VALUE is a TOML value, such as '"cristofd"', 'true' or '["0", "keep"]'. FILE is the
program's `file` as the manifest writes it. An existing KEY line in the entry is replaced;
otherwise the line is added after `file`.
"""

import re
import sys
import tomllib
from pathlib import Path


def main() -> int:
    if len(sys.argv) < 5:
        print(__doc__, file=sys.stderr)
        return 2
    manifest, key, value, *files = sys.argv[1:]
    tomllib.loads(f"x = {value}")             # reject a malformed value early
    path = Path(manifest)
    text = path.read_text()
    blocks = re.split(r"(?m)^(?=\[\[program\]\]$)", text)
    wanted = set(files)
    for i, block in enumerate(blocks):
        match = re.search(r'(?m)^file = "([^"]*)"$', block)
        if not match or match.group(1) not in wanted:
            continue
        wanted.discard(match.group(1))
        line = f"{key} = {value}"
        if re.search(rf"(?m)^{re.escape(key)} = ", block):
            block = re.sub(rf"(?m)^{re.escape(key)} = .*$", lambda _: line, block, count=1)
        else:
            block = block[:match.end()] + "\n" + line + block[match.end():]
        blocks[i] = block
    if wanted:
        print(f"not found in {manifest}: {', '.join(sorted(wanted))}", file=sys.stderr)
        return 1
    new = "".join(blocks)
    tomllib.loads(new)
    path.write_text(new)
    return 0


if __name__ == "__main__":
    sys.exit(main())
