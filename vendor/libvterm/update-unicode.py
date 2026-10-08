import sys
from pathlib import Path


def intervals(points):
    start = previous = None
    for point in sorted(points):
        if previous is not None and point != previous + 1:
            yield start, previous
            start = None
        if start is None:
            start = point
        previous = point
    if start is not None:
        yield start, previous


unicode_data = Path(sys.argv[1]).read_text(encoding="utf-8")
east_asian_width = Path(sys.argv[2]).read_text(encoding="utf-8")
combining = set(range(0x1160, 0x1200))
for line in unicode_data.splitlines():
    fields = line.split(";")
    if fields[2] in ("Mn", "Me", "Cf"):
        combining.add(int(fields[0], 16))
combining.discard(0xAD)
wide = set()
for line in east_asian_width.splitlines():
    fields = line.split("#", 1)[0].strip()
    if not fields:
        continue
    span, width = map(str.strip, fields.split(";"))
    if width not in ("W", "F"):
        continue
    bounds = span.split("..")
    wide.update(range(int(bounds[0], 16), int(bounds[-1], 16) + 1))
for name, points in (("combining", combining), ("fullwidth", wide - combining)):
    table = "".join(f"  {{ 0x{start:04X}, 0x{end:04X} }},\n"
                    for start, end in intervals(points))
    Path(__file__).with_name("src").joinpath(name + ".inc").write_text(
        table, encoding="utf-8", newline="\n"
    )
