# Brainfuck programs

A corpus of Brainfuck programs from many authors and sources, used to test Brainfunk and to
measure which instruction patterns real programs need. A wide corpus keeps those decisions
from fitting a handful of famous programs.

## Licensing

**The repository's public-domain dedication does not cover this directory or `3rdparty/`.**
Every program keeps the terms its author gave it. Each source directory holds that source's
own license and README files, unmodified, and a `SOURCE.md` with where and when the files
were obtained. When a program's file names its author, the manifest records that author too.

Programs whose license could not be established are not committed. `WANTED.md` lists them.

## Layout

```text
programs/
  README.md          this file
  WANTED.md          programs left out for now, and why
  index.tsv          generated: hashes, duplicate clusters and families of every program
  <source>/
    *.toml           manifest: the source, its license, and metadata for each program
    LICENSE ...      the source's own license files, unmodified
    SOURCE.md        where the files came from, and the license statement verbatim
    *.b              programs committed here, unmodified
3rdparty/<repo>/     git submodules for sources that have a git repository
tools/corpus/        the scripts that manage this directory
```

A source with a git repository is a submodule under `3rdparty/`, and its manifest names it
with `submodule = "3rdparty/..."`; its program paths are relative to the submodule. Files
generated from a submodule, such as `awib/awib.b`, are committed here with their generator
and the command that rebuilds them.

## Manifests

```toml
[source]
name = "brainfuck.org"
url = "https://brainfuck.org/"
license = "CC-BY-SA-4.0"           # SPDX identifier
license_files = ["LICENSE", "SOURCE.md"]
# submodule = "3rdparty/..."        # when the files live in a submodule

[defaults]                          # inherited by every [[program]] below
author = "Daniel B. Cristofani"
family = "cristofani"
origin = "handwritten"
role = "workload"
halts = "always"

[[program]]
file = "dbfi.b"
url = "https://brainfuck.org/dbfi.b"
halts = "with-input"
cases = [{ input = "inputs/dbfi/hello.in", expect = "inputs/dbfi/hello.out" }]
```

| Field | Values | Meaning |
|---|---|---|
| `family` | free text | The author or the generator. Statistics weight families equally, so one prolific author or tool cannot dominate. |
| `origin` | `handwritten`, `macro`, `compiled`, `textgen`, `unknown` | How the Brainfuck was produced. Generated code has very different patterns from handwritten code. |
| `role` | `workload`, `conformance`, `stress`, `fragment` | A real program; a test of implementation behaviour; a program meant to run (almost) forever; or a snippet that is not a program by itself. |
| `halts` | `always`, `with-input`, `interactive`, `never` | Halts with no input; needs input to do anything useful; needs a scripted session; does not finish in reasonable time. |
| `cell_bits` | `8`, `16`, `32`, `"any"` | The cell width the program is written for. Brainfunk has 8-bit cells. |
| `eof` | list of `"0"`, `"255"`, `"keep"` | End-of-input behaviours the program handles. Absent means unknown. |
| `tape_left` | boolean | Whether the program moves left of its starting cell. |
| `tape_cells` | integer | Cells the program needs, when that is more than usual. |
| `cases` | list of `{input, expect, expect_cell_bits}` | Inputs and expected outputs, relative to the source root. `expect_cell_bits` marks an expected output that only matches with wider cells. |
| `license_note` | text | A license statement copied verbatim from the source, when it is not a plain SPDX identifier. |
| `reviewed` | boolean | `false` on drafted entries whose metadata nobody has checked yet. |
| `license`, `author`, `notes`, `url`, `generated_from` | | Per-program details. |

## Tools

```sh
tools/corpus/corpus.py index     # regenerate programs/index.tsv after any change
tools/corpus/corpus.py verify    # check manifests, files, licenses and the index
tools/corpus/corpus.py fetch programs/<source>/programs.toml   # download files with a url
tools/corpus/corpus.py list --role workload --halts always --cell-bits 8 --unique
tools/corpus/corpus.py summary   # counts by source, role and origin
tools/corpus/corpus.py draft 3rdparty/<repo>/<dir> programs/<source>/<name>.toml
tools/corpus/setfield.py programs/<source>/<name>.toml family '"cristofd"' path/to/file.b
```

`draft` writes a manifest for a directory of a submodule, running each program once on empty
input to guess `halts`. Its entries say `reviewed = false` until someone checks them, and
`origin` stays `unknown`. When duplicates are clustered, reviewed entries name the cluster.
`verify` warns when a cluster mixes families, which usually means a program was attributed
to whoever collected it rather than whoever wrote it.

`index` normalizes every program to its eight commands and hashes it with BLAKE3. Programs
with the same normalized hash are exact duplicates. Programs whose run-length token 8-grams
have a Jaccard similarity of 0.9 or more are near duplicates. Both kinds join one cluster,
named after its first member, and `list --unique` keeps one program per cluster.

The tools need Python 3.11 or later with `blake3`, `click` and `tqdm`.
