#ifndef WOW_SOURCE_WORLDCLIENT_ADTCOMPAT_H
#define WOW_SOURCE_WORLDCLIENT_ADTCOMPAT_H

// Loads terrain tiles stored the 4.x way: one tile split into a root file (heights, normals, vertex
// colours, liquid), a _tex0 file (texture names, layers, alpha and shadow maps) and an _obj0 file (model
// names and placements, per-chunk object references).
//
// Not part of the original 12340 client. The three files are merged in memory into one tile laid out the
// way this client's own parser reads it (CMapArea::ProcessIffChunks, CMapChunk::ProcessIffChunks), so
// nothing downstream changes. The approach follows WarcraftXL's wxl-modern-adt (GPL-3.0-or-later,
// https://github.com/WarcraftXL).

#include <cstdint>

struct AdtCompatSource {
    const uint8_t* data;
    uint32_t size;
};

// Size of the merged tile, or 0 when the root file is not a usable tile.
uint32_t AdtCompatMergedSize(const AdtCompatSource& root, const AdtCompatSource& tex, const AdtCompatSource& obj);

// Writes the merged tile into out, which holds AdtCompatMergedSize bytes. tex and obj may be empty.
// Returns the bytes written, or 0 on malformed input.
uint32_t AdtCompatMerge(const AdtCompatSource& root, const AdtCompatSource& tex, const AdtCompatSource& obj, uint8_t* out, uint32_t outSize);

#endif
