"""The image loader against the SDRAM model: what it writes must be the image bfload packed."""
import random
import struct
import zlib

import pytest

from conftest import CORPUS, ROOT, assemble, run

SOURCES = ["tb/loader_tb.sv", "rtl/board/loader.sv", "rtl/brainfunk_mem.sv", "rtl/sdram_ctrl.sv", "tb/sdram_model.sv"]


def stress_words():
    """Words that copy from every kind of place: the line being filled, overlapping themselves, the
    line before, across a line boundary, and further back than 8192 words, up to 65535."""
    rng = random.Random(1)
    words = []

    def fresh(count):
        words.extend(rng.getrandbits(32) for _ in range(count))

    def repeat(distance, count):
        for _ in range(count):
            words.append(words[-distance])

    fresh(5)
    for distance in [1, 2, 3, 5, 7, 8, 9, 15, 16, 17]:
        for length in [2, 3, 9, 40, 129]:
            fresh(rng.randrange(1, 11))
            repeat(distance, length)
    fresh(9000)
    repeat(8500, 300)
    fresh(30000)
    repeat(30001, 200)
    fresh(30000)
    repeat(65535, 129)
    repeat(65000, 100)
    fresh(3)
    return words


def with_header(data, at, value):
    """The image file with one header byte changed, and its CRC made right again."""
    data = bytearray(data)
    data[at] = value
    struct.pack_into("<I", data, 16, zlib.crc32(bytes(data[4:16] + data[20:])))
    return bytes(data)


def pack(tmp_path, name, words, mode="packed", change=None):
    """The image, its image file's bytes as a file for $readmemh, and the bytes; mode bin is not compressed."""
    hexfile = tmp_path / f"{name}.hex"
    hexfile.write_text("".join(f"{word:08x}\n" for word in words))
    packed = tmp_path / f"{name}.{mode}"
    run(ROOT / "brainfunk", "-m", mode, "-b", hexfile, "-o", packed)
    data = packed.read_bytes()
    if change is not None:
        data = with_header(data, *change)
    (tmp_path / f"{name}.bytes").write_text("".join(f"{byte:02x}\n" for byte in data))
    return hexfile, tmp_path / f"{name}.bytes", data


def load(tmp_path, words, gap=0, corrupt_at=-1, before=None, sources=SOURCES, mode="packed", change=None):
    """Loads words, after the image before if there is one."""
    hexfile, bytesfile, data = pack(tmp_path, "image", words, mode, change)
    first = []
    if before is not None:
        _, pre_file, pre = pack(tmp_path, "before", before)
        first = [f"-GPRE_BYTES={len(pre)}", f'-GPRE_FILE="{pre_file}"']
    obj = tmp_path / "obj"
    run("verilator", "--binary", "--timing", "-j", "2", "--top-module", "loader_tb", "--Mdir", obj,
        f"-GIMAGE_BYTES={len(data)}", f'-GIMAGE_FILE="{bytesfile}"',
        f"-GWORDS={len(words)}", f'-GWORDS_FILE="{hexfile}"', f"-GGAP={gap}", f"-GCORRUPT_AT={corrupt_at}",
        f"-GREFUSE={int(corrupt_at >= 0 or change is not None)}",
        *first, *sources, "-o", "loader_tb")
    result = run(obj / "loader_tb", check=False, timeout=600)
    assert "loader_tb passed" in result.stdout, result.stdout[-2000:]
    return data, result.stdout


def copy_distances(data):
    """The distances of the copies in a packed image."""
    at, distances = 20, []
    while at < len(data):
        token = data[at]
        if token & 0x80:
            distances.append(data[at + 1] | data[at + 2] << 8)
            at += 3
        else:
            at += 1 + 4 * (token + 1)
    return distances


def test_loader_copies_from_anywhere(tools, tmp_path, sources=SOURCES):
    words = stress_words()
    data, _ = load(tmp_path, words, sources=sources)
    distances = copy_distances(data)
    assert min(distances) == 1 and max(distances) == 65535
    assert any(8192 <= distance < 65535 for distance in distances)


def test_loader_forgets_the_last_image(tools, tmp_path, sources=SOURCES):
    # The first image ends having read back its line 1, and the second's only copy is from its own.
    rng = random.Random(3)
    first = [rng.getrandbits(32) for _ in range(70)]
    first[68:70] = first[8:10]
    words = [rng.getrandbits(32) for _ in range(70)]
    words[60:62] = words[8:10]
    load(tmp_path, words, before=first, sources=sources)


def test_loader_with_a_slow_source(tools, tmp_path, sources=SOURCES):
    rng = random.Random(2)
    words = [rng.getrandbits(32) for _ in range(20)]
    for _ in range(40):
        words.extend(words[-rng.randrange(1, 20)] for _ in range(rng.randrange(2, 30)))
    load(tmp_path, words, gap=37, sources=sources)


def test_loader_takes_an_image_that_is_not_compressed(tools, tmp_path, sources=SOURCES):
    load(tmp_path, stress_words()[:3000], mode="bin", sources=sources)


# Right CRC, but something this loader cannot take: another version, compression or reserved bit, a
# payload size a plain image cannot have, more words than Code RAM.
@pytest.mark.parametrize("at,value", [(4, 2), (5, 2), (5, 0x81), (6, 1), (7, 0x80), (8, 1), (14, 0x10), (15, 1)],
                         ids=["version", "compression", "flag", "reserved", "reserved_high", "size", "words", "words_high"])
def test_loader_refuses_what_it_cannot_take(tools, tmp_path, at, value, sources=SOURCES):
    mode = "bin" if at == 8 else "packed"
    _, output = load(tmp_path, stress_words()[:300], sources=sources, mode=mode, change=(at, value))
    assert "refused" in output


# Header bytes: 4 the version, 5 flags, 6 reserved, 9 payload size, 13 word count, 17 CRC.
@pytest.mark.parametrize("at", [4, 5, 6, 9, 13, 17, -2], ids=["version", "flags", "reserved", "size", "words", "crc", "payload"])
def test_loader_refuses_a_corrupt_image(tools, tmp_path, at, sources=SOURCES):
    words = stress_words()[:3000]
    size = len(pack(tmp_path, "probe", words)[2])
    _, output = load(tmp_path, words, corrupt_at=at % size, sources=sources)
    # A larger payload size may also leave it waiting for the rest; nothing else may.
    assert "refused" in output or (at == 9 and "still waiting" in output)


def test_loader_loads_lost_kingdom(tools, tmp_path):
    source = CORPUS / "LostKng.b"
    if not source.exists():
        pytest.skip(f"{source} not found")
    image = assemble(source, 2, tmp_path / "lostkng.hex")
    words = [int(line, 16) for line in image.read_text().split()]
    data, _ = load(tmp_path, words)
    assert any(distance >= 8192 for distance in copy_distances(data))
