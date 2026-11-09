#!/usr/bin/env python3
"""Renders the WaveDrom sources in doc/diagrams/ to SVG beside them, which the docs link to; or checks
that the Mermaid diagrams in the docs parse.

    tools/render_diagrams.py [--check]
    tools/render_diagrams.py --mermaid

Needs python-wavedrom. The SVGs are committed, so that the docs show them without a build; run this
after changing a .json, and commit both. --check renders to memory and fails if any SVG is out of date.
WaveDrom draws black on nothing, which a dark page would hide, so each SVG gets a white background.

The Mermaid diagrams are written in the Markdown itself, and GitHub draws them, so there is nothing to
commit for them. --mermaid has mermaid-cli (mmdc) render every Markdown file that has one, into a
temporary directory, and fails if any does not parse.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIAGRAMS = ROOT / "doc" / "diagrams"
DOCS = [ROOT / "README.md", *sorted((ROOT / "doc").glob("*.md"))]


def render(source: Path) -> str:
    import wavedrom

    svg = wavedrom.render(source.read_text()).tostring()
    # The id counts up across renders in one process; pin it so that the output depends on the source alone.
    svg = re.sub(r'id="svgcontent_\d+"', 'id="svgcontent_0"', svg, count=1)
    opening = re.search(r"<svg[^>]*>", svg)
    assert opening, "WaveDrom gave no <svg>"
    return svg[:opening.end()] + '<rect width="100%" height="100%" fill="#fff" />' + svg[opening.end():] + "\n"


def wavedrom_diagrams(check: bool) -> int:
    stale = []
    for source in sorted(DIAGRAMS.glob("*.json")):
        target = source.with_suffix(".svg")
        svg = render(source)
        if check:
            if not target.exists() or target.read_text() != svg:
                stale.append(target.name)
        else:
            target.write_text(svg)
            print(target.relative_to(ROOT))
    for name in stale:
        print(f"out of date: {name}", file=sys.stderr)
    return 1 if stale else 0


def mermaid_diagrams() -> int:
    failed = 0
    with tempfile.TemporaryDirectory() as out:
        for doc in DOCS:
            blocks = doc.read_text().count("```mermaid")
            if not blocks:
                continue
            result = subprocess.run(["mmdc", "-q", "-i", str(doc), "-o", str(Path(out) / doc.name)],
                                    capture_output=True, text=True, stdin=subprocess.DEVNULL)
            name = doc.relative_to(ROOT)
            if result.returncode != 0:
                failed += 1
                print(f"{name}: does not render\n{result.stderr}", file=sys.stderr)
            else:
                print(f"{name}: {blocks} Mermaid diagram{'s' if blocks > 1 else ''}")
    return 1 if failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    what = parser.add_mutually_exclusive_group()
    what.add_argument("--check", action="store_true", help="fail if an SVG differs from its WaveDrom source")
    what.add_argument("--mermaid", action="store_true", help="fail if a Mermaid diagram in the docs does not parse")
    args = parser.parse_args()
    return mermaid_diagrams() if args.mermaid else wavedrom_diagrams(args.check)


if __name__ == "__main__":
    sys.exit(main())
