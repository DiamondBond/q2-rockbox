# Simplified Chinese fonts for Rockbox

These 13 RB12 `.fnt` files were compiled using this repository's
`tools/convbdf.c` from the corresponding BDF sources in `fonts/`.
The `zh-cn-` filename prefix identifies fonts with full coverage of the
6,763 Chinese characters in GB2312; these fonts also contain other scripts.

Copy the `.fnt` files into `.rockbox/fonts/` on the player and select a font
in Rockbox's font settings.

Included fonts:

- 09, 10, 11 and 12 WenQangYi-Song, regular and bold (8 files)
- 14-Rockbox-Mix
- 16-GNU-Unifont
- 16-WenQangYi-Unibit
- 18-Fixed and 18-Fixed-Bold

Conversion uses the default ascent/descent without scaling or subsetting.
Numbers in source filenames are retained and do not necessarily indicate
the resulting pixel height. Some non-Chinese glyphs extend beyond the
source font metrics and are clipped by the standard converter.
The original Rockbox-Mix conversion also retains three non-Chinese glyph
widths exceeding its declared maximum width.

Validation checks RB12 headers, file lengths, glyph offsets and widths,
and nonempty bitmaps for all 6,763 GB2312 Chinese characters in each file.
The eight Song fonts also passed pixel comparisons against their BDF CJK
glyphs. No on-device rendering test has been performed.

The original font licenses and copyrights remain applicable; see the
corresponding BDF source headers and `../COPYING`.

Example conversion (run from the repository root with a built convbdf):

```sh
tools/convbdf -f -o fonts/zh-cn/zh-cn-09-WenQangYi-Song.fnt fonts/09-WenQangYi-Song.bdf
```
