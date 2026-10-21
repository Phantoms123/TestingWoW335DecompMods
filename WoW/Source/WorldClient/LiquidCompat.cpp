#include "WoW/Source/WorldClient/LiquidCompat.h"
#include <cstring>
#include <vector>
#include "WoW/Source/LiquidSystem/Types.h"

static_assert(sizeof(SMLiquidChunkDesc) == 12, "MH2O chunk header");
static_assert(sizeof(SMLiquidInstance) == 24, "MH2O layer");

namespace {

const uint32_t CHUNK_COUNT = 256;
// Below this the field is a liquid vertex format; from it on, a LiquidObject id (4.3.4 Liquid::GetVertexFormat).
const uint16_t FIRST_LIQUID_OBJECT = 42;
const uint16_t LIQUID_TYPE_OCEAN = 2;

// Vertex layouts. This client reads 0 (heights, then depths), 1 (heights, then texture coordinates) and
// 2 (depths only, or none). 4.x adds 3 (heights, texture coordinates, depths), 4 (per vertex: depth, 3 unused
// bytes, height) and 5 (per vertex: texture coordinate, height).
enum Lvf { LVF_HEIGHT_DEPTH = 0, LVF_HEIGHT_UV = 1, LVF_DEPTH = 2, LVF_HEIGHT_UV_DEPTH = 3, LVF_DEPTH_HEIGHT_PACKED = 4, LVF_UV_HEIGHT_PACKED = 5 };

// The 4.3.4 LiquidType table: id, LiquidMaterial id, sound bank. The vertex data was written for these
// materials, so they decide its layout even where this client's own table differs.
struct CataLiquidType {
    uint16_t id;
    uint8_t material;
    uint8_t soundBank;
};

const CataLiquidType s_cataLiquidTypes[] = {
    { 1, 1, 0 }, { 2, 1, 1 }, { 3, 2, 2 }, { 4, 2, 3 }, { 5, 1, 0 }, { 6, 1, 1 }, { 7, 2, 2 }, { 8, 2, 3 },
    { 9, 1, 0 }, { 10, 1, 1 }, { 11, 2, 2 }, { 12, 2, 3 }, { 13, 1, 0 }, { 14, 1, 1 }, { 15, 2, 2 }, { 17, 1, 0 },
    { 19, 2, 2 }, { 20, 2, 3 }, { 21, 2, 3 }, { 41, 1, 0 }, { 61, 1, 0 }, { 81, 1, 0 }, { 100, 3, 1 }, { 101, 3, 1 },
    { 121, 2, 2 }, { 141, 2, 2 }, { 181, 1, 0 }, { 221, 3, 0 }, { 241, 1, 0 }, { 301, 1, 0 }, { 302, 4, 2 },
    { 303, 4, 2 }, { 321, 3, 1 }, { 324, 3, 1 }, { 350, 5, 1 }, { 397, 2, 2 }, { 404, 2, 2 }, { 406, 8, 0 },
    { 407, 1, 0 }, { 411, 1, 0 }, { 412, 5, 1 }, { 671, 2, 2 }, { 689, 1, 0 }, { 739, 2, 2 },
};

// The 4.3.4 LiquidMaterial table: id and its vertex format.
struct CataLiquidMaterial {
    uint8_t id;
    uint8_t lvf;
};

const CataLiquidMaterial s_cataLiquidMaterials[] = { { 1, 0 }, { 2, 1 }, { 3, 0 }, { 4, 1 }, { 5, 0 }, { 8, 0 } };

// The stock type that stands in for each sound bank: water, ocean, magma, slime.
const uint16_t s_stockTypeBySoundBank[] = { 1, 2, 3, 4 };

const CataLiquidType* FindCataType(uint32_t id) {
    for (const CataLiquidType& type : s_cataLiquidTypes) {
        if (type.id == id) {
            return &type;
        }
    }

    return nullptr;
}

int32_t CataMaterialLvf(uint32_t material) {
    for (const CataLiquidMaterial& m : s_cataLiquidMaterials) {
        if (m.id == material) {
            return m.lvf;
        }
    }

    return -1;
}

// The layer's real vertex format, as the 4.3.4 client resolves it; -1 when it cannot be known.
int32_t ResolveLvf(uint16_t liquidType, uint16_t field) {
    if (field < FIRST_LIQUID_OBJECT) {
        return field;
    }

    if (liquidType == LIQUID_TYPE_OCEAN) {
        return LVF_DEPTH;
    }

    const CataLiquidType* cata = FindCataType(liquidType);
    return cata ? CataMaterialLvf(cata->material) : -1;
}

uint32_t VertexBytes(int32_t lvf, uint32_t count) {
    switch (lvf) {
        case LVF_HEIGHT_DEPTH: return 5 * count;
        case LVF_HEIGHT_UV: return 8 * count;
        case LVF_DEPTH: return count;
        case LVF_HEIGHT_UV_DEPTH: return 9 * count;
        case LVF_DEPTH_HEIGHT_PACKED:
        case LVF_UV_HEIGHT_PACKED: return 8 * count;
        default: return 0;
    }
}

// Per-vertex depth byte and height float -> heights for every vertex, then depths for every vertex.
void UnpackDepthHeight(uint8_t* v, uint32_t count) {
    std::vector<uint8_t> packed(v, v + 8 * count);

    for (uint32_t i = 0; i < count; i++) {
        memcpy(v + 4 * i, &packed[8 * i + 4], 4);
        v[4 * count + i] = packed[8 * i];
    }
}

// Per-vertex texture coordinate and height float -> heights for every vertex, then texture coordinates.
void UnpackUvHeight(uint8_t* v, uint32_t count) {
    std::vector<uint8_t> packed(v, v + 8 * count);

    for (uint32_t i = 0; i < count; i++) {
        memcpy(v + 4 * i, &packed[8 * i + 4], 4);
        memcpy(v + 4 * count + 4 * i, &packed[8 * i], 4);
    }
}

}

bool LiquidCompatNormalize(uint8_t* data, uint32_t size, const LiquidCompatClient& client, LiquidCompatReport* report) {
    LiquidCompatReport r = {};

    if (size < CHUNK_COUNT * sizeof(SMLiquidChunkDesc)) {
        return false;
    }

    for (uint32_t c = 0; c < CHUNK_COUNT; c++) {
        SMLiquidChunkDesc chunk;
        memcpy(&chunk, data + c * sizeof(chunk), sizeof(chunk));

        if (!chunk.m_layerCount) {
            continue;
        }

        if (chunk.m_offsetInstances > size || chunk.m_layerCount > (size - chunk.m_offsetInstances) / sizeof(SMLiquidInstance)) {
            return false;
        }

        for (uint32_t l = 0; l < chunk.m_layerCount; l++) {
            uint8_t* at = data + chunk.m_offsetInstances + l * sizeof(SMLiquidInstance);
            SMLiquidInstance layer;
            memcpy(&layer, at, sizeof(layer));
            r.layers++;

            uint32_t vertexCount = (layer.m_width + 1u) * (layer.m_height + 1u);
            int32_t lvf = ResolveLvf(layer.m_liquidType, layer.m_vertexFormat);

            if (layer.m_vertexFormat >= FIRST_LIQUID_OBJECT) {
                r.objectFormatsResolved++;
            }

            uint32_t bytes = VertexBytes(lvf, vertexCount);
            bool dataInside = layer.m_offsetVertexData && layer.m_offsetVertexData <= size && bytes <= size - layer.m_offsetVertexData;

            if (lvf < 0 || (lvf != LVF_DEPTH && !dataInside)) {
                // Nothing can be read safely: a flat surface at the layer's height range.
                lvf = LVF_DEPTH;
                layer.m_offsetVertexData = 0;
            } else if (lvf == LVF_DEPTH && layer.m_offsetVertexData && !dataInside) {
                layer.m_offsetVertexData = 0;
            }

            uint8_t* vertices = data + layer.m_offsetVertexData;

            switch (lvf) {
                case LVF_HEIGHT_UV_DEPTH:
                    // Its leading heights and texture coordinates are exactly the format 1 layout.
                    lvf = LVF_HEIGHT_UV;
                    break;
                case LVF_DEPTH_HEIGHT_PACKED:
                    UnpackDepthHeight(vertices, vertexCount);
                    lvf = LVF_HEIGHT_DEPTH;
                    r.verticesRewritten += vertexCount;
                    break;
                case LVF_UV_HEIGHT_PACKED:
                    UnpackUvHeight(vertices, vertexCount);
                    lvf = LVF_HEIGHT_UV;
                    r.verticesRewritten += vertexCount;
                    break;
                case LVF_HEIGHT_DEPTH:
                case LVF_HEIGHT_UV:
                case LVF_DEPTH:
                    break;
                default:
                    lvf = LVF_DEPTH;
                    layer.m_offsetVertexData = 0;
                    break;
            }

            layer.m_vertexFormat = static_cast<uint16_t>(lvf);

            if (!client.hasLiquidType(layer.m_liquidType)) {
                const CataLiquidType* cata = FindCataType(layer.m_liquidType);
                uint32_t bank = cata && cata->soundBank < 4 ? cata->soundBank : 0;
                layer.m_liquidType = s_stockTypeBySoundBank[bank];
                r.typesReplaced++;
            }

            memcpy(at, &layer, sizeof(layer));
        }
    }

    if (report) {
        *report = r;
    }

    return true;
}
