These files are unmodified official Unicode 16.0.0 character data:

- [UnicodeData.txt](https://www.unicode.org/Public/16.0.0/ucd/UnicodeData.txt)
- [EastAsianWidth.txt](https://www.unicode.org/Public/16.0.0/ucd/EastAsianWidth.txt)
- [Unicode License V3](https://www.unicode.org/license.txt), copied to LICENSE.txt

`python tools/generate-unicode.py` regenerates `native/unicode-width.hpp`.
`python tools/generate-unicode.py --check` verifies the checked-in header.
The header records the SHA-256 digest of each source file.

Terminal policy: Mn, Me, and Cf characters and Hangul Jamo U+1160–U+11FF
occupy zero cells, except soft hyphen U+00AD, which occupies one cell.
East Asian Width W and F characters occupy two cells unless zero-width.
All other characters occupy one cell. The terminal parser handles controls.
This is a code-point width policy, not grapheme-cluster segmentation.
