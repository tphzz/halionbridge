# dxsyx provenance

This directory contains the small portion of dxsyx needed by halionbridge's Yamaha DX7 converter.

- Upstream project: https://github.com/rogerallen/dxsyx
- Upstream commit: `efb54c043ca2791dfb571e54263df4954491509c`
- Original author: Roger Allen
- Original license: GNU General Public License version 3 or later; see `LICENSE.txt`
- Source basis: the DX7 bank layout and decoding logic in upstream `dxsyx/dxsyx.h` and `dxsyx/dxsyx.cpp`

The halionbridge adaptation removes the executable, file I/O, global configuration, stream output, database/breeding behavior, and DX7 Mk2 conversion. It adds immutable byte-span parsing, framed single-voice and raw-bank support, strict message framing/checksums, typed C++23 value models, structured diagnostics, parameter-range normalization, concatenated-message parsing, and an explicit recovery mode that resynchronizes at SysEx frame boundaries without repairing invalid data.

The adapted source files retain Roger Allen's copyright and GPL notice. halionbridge-specific converter orchestration and HALion mapping do not live in this directory.
