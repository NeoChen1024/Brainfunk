# awib

[awib](https://github.com/matslina/awib) by Mats Linander is a Brainfuck compiler written in
Brainfuck. The repository is the submodule `3rdparty/awib`, licensed GPL-3.0 or later; its
`COPYING` is copied here as `LICENSE`.

## awib.b

The repository keeps only the sources of awib, so `awib.b` is built from them and committed
here. It was built from submodule commit `f9f43a0092b6c4ba77c6d2d9e21db3b3b37cd706`
(2025-02-16) with the command from awib's own `Makefile`, run inside `3rdparty/awib`:

```sh
python3 util/bfpp.py --interpreter --format formats/awib-0.3 --width 79 awib-skeleton.b > awib.b
```

`awib.b` is unmodified output of that command, and remains under the GPL.

## systest.toml

awib's system tests are listed in `systest.toml` and read from the submodule in place. The
`factor.b` test there is by Brian Raiter, and its file states that it is under the GNU General
Public License.
