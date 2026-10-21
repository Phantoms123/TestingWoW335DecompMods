#include "WoW/Source/WorldClient/AdtCompat.h"
#include <cstring>
#include "WoW/Source/WorldClient/MapArea.h"
#include "WoW/Source/WorldClient/MapChunk.h"

// Chunk tokens as this client compares them: the little-endian dword read of the on-disk bytes, which
// are stored reversed ("REVM" on disk reads back as 'MVER').
#define ADT_TOKEN(a, b, c, d) ((uint32_t(a) << 24) | (uint32_t(b) << 16) | (uint32_t(c) << 8) | uint32_t(d))

static const uint32_t TOKEN_MVER = ADT_TOKEN('M', 'V', 'E', 'R');
static const uint32_t TOKEN_MHDR = ADT_TOKEN('M', 'H', 'D', 'R');
static const uint32_t TOKEN_MCIN = ADT_TOKEN('M', 'C', 'I', 'N');
static const uint32_t TOKEN_MTEX = ADT_TOKEN('M', 'T', 'E', 'X');
static const uint32_t TOKEN_MMDX = ADT_TOKEN('M', 'M', 'D', 'X');
static const uint32_t TOKEN_MMID = ADT_TOKEN('M', 'M', 'I', 'D');
static const uint32_t TOKEN_MWMO = ADT_TOKEN('M', 'W', 'M', 'O');
static const uint32_t TOKEN_MWID = ADT_TOKEN('M', 'W', 'I', 'D');
static const uint32_t TOKEN_MDDF = ADT_TOKEN('M', 'D', 'D', 'F');
static const uint32_t TOKEN_MODF = ADT_TOKEN('M', 'O', 'D', 'F');
static const uint32_t TOKEN_MH2O = ADT_TOKEN('M', 'H', '2', 'O');
static const uint32_t TOKEN_MFBO = ADT_TOKEN('M', 'F', 'B', 'O');
static const uint32_t TOKEN_MTXF = ADT_TOKEN('M', 'T', 'X', 'F');
static const uint32_t TOKEN_MAMP = ADT_TOKEN('M', 'A', 'M', 'P');
static const uint32_t TOKEN_MCNK = ADT_TOKEN('M', 'C', 'N', 'K');
static const uint32_t TOKEN_MCVT = ADT_TOKEN('M', 'C', 'V', 'T');
static const uint32_t TOKEN_MCCV = ADT_TOKEN('M', 'C', 'C', 'V');
static const uint32_t TOKEN_MCNR = ADT_TOKEN('M', 'C', 'N', 'R');
static const uint32_t TOKEN_MCLY = ADT_TOKEN('M', 'C', 'L', 'Y');
static const uint32_t TOKEN_MCRF = ADT_TOKEN('M', 'C', 'R', 'F');
static const uint32_t TOKEN_MCRD = ADT_TOKEN('M', 'C', 'R', 'D');
static const uint32_t TOKEN_MCRW = ADT_TOKEN('M', 'C', 'R', 'W');
static const uint32_t TOKEN_MCSH = ADT_TOKEN('M', 'C', 'S', 'H');
static const uint32_t TOKEN_MCAL = ADT_TOKEN('M', 'C', 'A', 'L');
static const uint32_t TOKEN_MCLQ = ADT_TOKEN('M', 'C', 'L', 'Q');
static const uint32_t TOKEN_MCSE = ADT_TOKEN('M', 'C', 'S', 'E');

static const uint32_t ADT_VERSION = 18;
static const uint32_t CHUNK_COUNT = 256;
static const uint32_t CHUNK_HEADER = 8;
// The client reads 448 normal bytes per chunk (145 normals plus padding) whatever the file states.
static const uint32_t MCNR_SIZE = 448;
// MCNK header flag: the chunk has a shadow map.
static const uint32_t MCNK_FLAG_HAS_MCSH = 0x1;
// MHDR flag: the tile has flight bounds.
static const uint32_t MHDR_FLAG_HAS_MFBO = 0x1;

static_assert(sizeof(SMChunk) == 0x80, "MCNK header");
static_assert(sizeof(SMAreaHeader) == 0x40, "MHDR");
static_assert(sizeof(SMChunkInfo) == 0x10, "MCIN entry");

namespace {

struct Span {
    const uint8_t* data;
    uint32_t size;
    bool present;
};

uint32_t Read32(const uint8_t* p) {
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

// The chunks of one file: the first of each top-level token, and every MCNK in order.
struct FileChunks {
    Span top[16];
    uint32_t topTokens[16];
    uint32_t topCount;
    Span chunks[CHUNK_COUNT];
    uint32_t chunkCount;

    void Scan(const AdtCompatSource& file) {
        memset(this, 0, sizeof(*this));

        uint32_t offset = 0;

        while (file.data && offset + CHUNK_HEADER <= file.size) {
            uint32_t token = Read32(file.data + offset);
            uint32_t size = Read32(file.data + offset + 4);

            if (size > file.size - offset - CHUNK_HEADER) {
                break;
            }

            Span span = { file.data + offset + CHUNK_HEADER, size, true };

            if (token == TOKEN_MCNK) {
                if (this->chunkCount < CHUNK_COUNT) {
                    this->chunks[this->chunkCount++] = span;
                }
            } else if (!this->Find(token).present && this->topCount < 16) {
                this->topTokens[this->topCount] = token;
                this->top[this->topCount++] = span;
            }

            offset += CHUNK_HEADER + size;
        }
    }

    Span Find(uint32_t token) const {
        for (uint32_t i = 0; i < this->topCount; i++) {
            if (this->topTokens[i] == token) {
                return this->top[i];
            }
        }

        Span none = { nullptr, 0, false };
        return none;
    }
};

// Finds a subchunk inside one MCNK body. skip is 0x80 for a root chunk (which starts with the header)
// and 0 for _tex0/_obj0 chunks (which have none).
Span FindSub(const Span& chunk, uint32_t skip, uint32_t token) {
    Span none = { nullptr, 0, false };

    if (!chunk.present || chunk.size < skip) {
        return none;
    }

    uint32_t offset = skip;

    while (offset + CHUNK_HEADER <= chunk.size) {
        uint32_t subToken = Read32(chunk.data + offset);
        uint32_t size = Read32(chunk.data + offset + 4);
        uint32_t available = chunk.size - offset - CHUNK_HEADER;

        if (subToken == token) {
            Span span = { chunk.data + offset + CHUNK_HEADER, size > available ? available : size, true };
            return span;
        }

        if (size > available) {
            break;
        }

        offset += CHUNK_HEADER + size;
    }

    return none;
}

// Sequential writer. With no output buffer it only counts, which sizes the merge.
struct Writer {
    uint8_t* out;
    uint32_t cap;
    uint32_t pos;
    bool overflow;

    void Bytes(const void* data, uint32_t size) {
        if (this->out) {
            if (size > this->cap - this->pos) {
                this->overflow = true;
                return;
            }

            if (data) {
                memcpy(this->out + this->pos, data, size);
            } else {
                memset(this->out + this->pos, 0, size);
            }
        }

        this->pos += size;
    }

    void U32(uint32_t value) {
        this->Bytes(&value, sizeof(value));
    }

    // Writes a chunk header followed by size bytes of data (zeros when data is null) and returns the
    // position of its token.
    uint32_t Chunk(uint32_t token, const void* data, uint32_t size) {
        uint32_t at = this->pos;
        this->U32(token);
        this->U32(size);
        this->Bytes(data, size);
        return at;
    }

    // Copies a record already written at position into value, so it can be patched. Records can sit at
    // any byte boundary (texture name lists have arbitrary lengths), so they are never accessed in place.
    template <class T>
    bool Get(uint32_t position, T& value) const {
        if (!this->out || this->overflow || position + sizeof(T) > this->pos) {
            return false;
        }

        memcpy(&value, this->out + position, sizeof(T));
        return true;
    }

    template <class T>
    void Put(uint32_t position, const T& value) {
        memcpy(this->out + position, &value, sizeof(T));
    }
};

// Prefers the split file's copy of a top-level chunk, falling back to the root file.
Span Pick(const FileChunks& preferred, const FileChunks& root, uint32_t token) {
    Span span = preferred.Find(token);
    return span.present ? span : root.Find(token);
}

Span PickSub(const Span& preferred, const Span& root, uint32_t token) {
    Span span = FindSub(preferred, 0, token);
    return span.present ? span : FindSub(root, sizeof(SMChunk), token);
}

uint32_t WriteChunk(Writer& w, uint32_t index, const FileChunks& root, const FileChunks& tex, const FileChunks& obj) {
    const Span& rootChunk = root.chunks[index];
    Span texChunk = index < tex.chunkCount ? tex.chunks[index] : Span { nullptr, 0, false };
    Span objChunk = index < obj.chunkCount ? obj.chunks[index] : Span { nullptr, 0, false };

    uint32_t start = w.Chunk(TOKEN_MCNK, nullptr, 0);
    uint32_t headerAt = w.pos;
    w.Bytes(rootChunk.data, sizeof(SMChunk));

    Span mcvt = FindSub(rootChunk, sizeof(SMChunk), TOKEN_MCVT);
    Span mccv = FindSub(rootChunk, sizeof(SMChunk), TOKEN_MCCV);
    Span mcnr = FindSub(rootChunk, sizeof(SMChunk), TOKEN_MCNR);
    Span mcse = FindSub(rootChunk, sizeof(SMChunk), TOKEN_MCSE);
    Span mclq = FindSub(rootChunk, sizeof(SMChunk), TOKEN_MCLQ);
    Span mcly = PickSub(texChunk, rootChunk, TOKEN_MCLY);
    Span mcsh = PickSub(texChunk, rootChunk, TOKEN_MCSH);
    Span mcal = PickSub(texChunk, rootChunk, TOKEN_MCAL);
    Span mcrd = FindSub(objChunk, 0, TOKEN_MCRD);
    Span mcrw = FindSub(objChunk, 0, TOKEN_MCRW);
    Span mcrf = FindSub(rootChunk, sizeof(SMChunk), TOKEN_MCRF);

    uint32_t ofsHeight = mcvt.present ? w.Chunk(TOKEN_MCVT, mcvt.data, mcvt.size) - start : 0;
    uint32_t ofsMCCV = mccv.present ? w.Chunk(TOKEN_MCCV, mccv.data, mccv.size) - start : 0;

    uint32_t ofsNormal = 0;

    if (mcnr.present) {
        ofsNormal = w.pos - start;
        w.U32(TOKEN_MCNR);
        w.U32(MCNR_SIZE);
        uint32_t copy = mcnr.size < MCNR_SIZE ? mcnr.size : MCNR_SIZE;
        w.Bytes(mcnr.data, copy);
        w.Bytes(nullptr, MCNR_SIZE - copy);
    }

    uint32_t ofsLayer = mcly.present ? w.Chunk(TOKEN_MCLY, mcly.data, mcly.size) - start : 0;

    // This client keeps doodad and map object references in one list, doodads first.
    uint32_t doodadRefs;
    uint32_t mapObjRefs;
    uint32_t ofsRefs = w.pos - start;

    if (mcrd.present || mcrw.present) {
        doodadRefs = mcrd.size / 4;
        mapObjRefs = mcrw.size / 4;
        w.U32(TOKEN_MCRF);
        w.U32((doodadRefs + mapObjRefs) * 4);
        w.Bytes(mcrd.data, doodadRefs * 4);
        w.Bytes(mcrw.data, mapObjRefs * 4);
    } else {
        const SMChunk* rootHeader = reinterpret_cast<const SMChunk*>(rootChunk.data);
        doodadRefs = mcrf.present ? rootHeader->m_nDoodadRefs : 0;
        mapObjRefs = mcrf.present ? rootHeader->m_nMapObjRefs : 0;
        w.Chunk(TOKEN_MCRF, mcrf.data, mcrf.present ? mcrf.size : 0);
    }

    uint32_t ofsShadow = mcsh.present ? w.Chunk(TOKEN_MCSH, mcsh.data, mcsh.size) - start : 0;
    uint32_t ofsAlpha = mcal.present ? w.Chunk(TOKEN_MCAL, mcal.data, mcal.size) - start : 0;
    uint32_t ofsLiquid = mclq.present ? w.Chunk(TOKEN_MCLQ, mclq.data, mclq.size) - start : 0;
    uint32_t ofsSndEmitters = mcse.present ? w.Chunk(TOKEN_MCSE, mcse.data, mcse.size) - start : 0;

    uint32_t end = w.pos;

    uint32_t chunkSize;

    if (w.Get(start + 4, chunkSize)) {
        w.Put<uint32_t>(start + 4, end - start - CHUNK_HEADER);
    }

    SMChunk headerValue;

    if (w.Get(headerAt, headerValue)) {
        SMChunk* header = &headerValue;
        header->m_nLayers = mcly.size / 16;
        header->m_nDoodadRefs = doodadRefs;
        header->m_nMapObjRefs = mapObjRefs;
        header->m_ofsHeight = ofsHeight;
        header->m_ofsNormal = ofsNormal;
        header->m_ofsLayer = ofsLayer;
        header->m_ofsRefs = ofsRefs;
        header->m_ofsAlpha = ofsAlpha;
        header->m_sizeAlpha = mcal.present ? mcal.size + CHUNK_HEADER : 0;
        header->m_ofsShadow = ofsShadow;
        header->m_sizeShadow = mcsh.present ? mcsh.size : 0;
        header->m_ofsLiquid = ofsLiquid;
        header->m_sizeLiquid = mclq.present ? mclq.size + CHUNK_HEADER : 0;
        header->m_ofsSndEmitters = ofsSndEmitters;
        header->m_ofsMCCV = ofsMCCV;

        if (mcsh.present) {
            header->m_flags |= MCNK_FLAG_HAS_MCSH;
        } else {
            header->m_flags &= ~MCNK_FLAG_HAS_MCSH;
        }

        w.Put(headerAt, headerValue);
    }

    return start;
}

uint32_t Merge(const AdtCompatSource& rootFile, const AdtCompatSource& texFile, const AdtCompatSource& objFile, uint8_t* out, uint32_t outSize) {
    FileChunks root;
    FileChunks tex;
    FileChunks obj;
    root.Scan(rootFile);
    tex.Scan(texFile);
    obj.Scan(objFile);

    Span rootHeader = root.Find(TOKEN_MHDR);

    if (!rootHeader.present || rootHeader.size < sizeof(SMAreaHeader) || root.chunkCount != CHUNK_COUNT) {
        return 0;
    }

    for (uint32_t i = 0; i < CHUNK_COUNT; i++) {
        if (root.chunks[i].size < sizeof(SMChunk)) {
            return 0;
        }
    }

    Writer w = { out, outSize, 0, false };

    w.Chunk(TOKEN_MVER, &ADT_VERSION, sizeof(ADT_VERSION));

    uint32_t headerChunk = w.Chunk(TOKEN_MHDR, rootHeader.data, sizeof(SMAreaHeader));
    uint32_t headerBase = headerChunk + CHUNK_HEADER;

    uint32_t mcin = w.Chunk(TOKEN_MCIN, nullptr, CHUNK_COUNT * sizeof(SMChunkInfo));

    Span mtex = Pick(tex, root, TOKEN_MTEX);
    Span mmdx = Pick(obj, root, TOKEN_MMDX);
    Span mmid = Pick(obj, root, TOKEN_MMID);
    Span mwmo = Pick(obj, root, TOKEN_MWMO);
    Span mwid = Pick(obj, root, TOKEN_MWID);
    Span mddf = Pick(obj, root, TOKEN_MDDF);
    Span modf = Pick(obj, root, TOKEN_MODF);
    Span mh2o = root.Find(TOKEN_MH2O);
    Span mfbo = root.Find(TOKEN_MFBO);
    Span mtxf = Pick(tex, root, TOKEN_MTXF);
    Span mamp = tex.Find(TOKEN_MAMP);

    // The client reads every one of these through MHDR, so each gets a chunk even when empty.
    uint32_t ofsMtex = w.Chunk(TOKEN_MTEX, mtex.data, mtex.size);
    uint32_t ofsMmdx = w.Chunk(TOKEN_MMDX, mmdx.data, mmdx.size);
    uint32_t ofsMmid = w.Chunk(TOKEN_MMID, mmid.data, mmid.size);
    uint32_t ofsMwmo = w.Chunk(TOKEN_MWMO, mwmo.data, mwmo.size);
    uint32_t ofsMwid = w.Chunk(TOKEN_MWID, mwid.data, mwid.size);
    uint32_t ofsMddf = w.Chunk(TOKEN_MDDF, mddf.data, mddf.size);
    uint32_t ofsModf = w.Chunk(TOKEN_MODF, modf.data, modf.size);
    // MH2O offsets are relative to its own data, so it copies as it is.
    uint32_t ofsMh2o = mh2o.present ? w.Chunk(TOKEN_MH2O, mh2o.data, mh2o.size) : 0;

    uint32_t chunkStarts[CHUNK_COUNT];

    for (uint32_t i = 0; i < CHUNK_COUNT; i++) {
        chunkStarts[i] = WriteChunk(w, i, root, tex, obj);
    }

    uint32_t ofsMfbo = mfbo.present ? w.Chunk(TOKEN_MFBO, mfbo.data, mfbo.size) : 0;
    uint32_t ofsMtxf = mtxf.present ? w.Chunk(TOKEN_MTXF, mtxf.data, mtxf.size) : 0;

    if (w.overflow) {
        return 0;
    }

    SMAreaHeader headerValue;

    if (w.Get(headerBase, headerValue)) {
        SMAreaHeader* header = &headerValue;
        header->m_mcin = mcin - headerBase;
        header->m_mtex = ofsMtex - headerBase;
        header->m_mmdx = ofsMmdx - headerBase;
        header->m_mmid = ofsMmid - headerBase;
        header->m_mwmo = ofsMwmo - headerBase;
        header->m_mwid = ofsMwid - headerBase;
        header->m_mddf = ofsMddf - headerBase;
        header->m_modf = ofsModf - headerBase;
        header->m_mh2o = ofsMh2o ? ofsMh2o - headerBase : 0;
        header->m_mfbo = ofsMfbo ? ofsMfbo - headerBase : 0;
        header->m_mtxf = ofsMtxf ? ofsMtxf - headerBase : 0;

        if (ofsMfbo) {
            header->m_flags |= MHDR_FLAG_HAS_MFBO;
        } else {
            header->m_flags &= ~MHDR_FLAG_HAS_MFBO;
        }

        if (mamp.present && mamp.size) {
            header->m_mampValue = mamp.data[0];
        }

        w.Put(headerBase, headerValue);
    }

    if (w.out) {
        for (uint32_t i = 0; i < CHUNK_COUNT; i++) {
            uint32_t next = i + 1 < CHUNK_COUNT ? chunkStarts[i + 1] : (ofsMfbo ? ofsMfbo : (ofsMtxf ? ofsMtxf : w.pos));
            SMChunkInfo info = {};
            info.m_offset = chunkStarts[i];
            info.m_size = next - chunkStarts[i];
            w.Put(mcin + CHUNK_HEADER + i * sizeof(SMChunkInfo), info);
        }
    }

    return w.pos;
}

}

uint32_t AdtCompatMergedSize(const AdtCompatSource& root, const AdtCompatSource& tex, const AdtCompatSource& obj) {
    return Merge(root, tex, obj, nullptr, 0);
}

uint32_t AdtCompatMerge(const AdtCompatSource& root, const AdtCompatSource& tex, const AdtCompatSource& obj, uint8_t* out, uint32_t outSize) {
    return Merge(root, tex, obj, out, outSize);
}
