#ifndef WOW_SOURCE_WORLDCLIENT_LIQUIDCOMPAT_H
#define WOW_SOURCE_WORLDCLIENT_LIQUIDCOMPAT_H

// Makes a tile's MH2O liquid authored for 4.x readable by this client, in place.
//
// Not part of the original 12340 client. 4.x stores a LiquidObject id (42 and up) where this client expects the
// liquid vertex format (LVF), and 4.x materials add two vertex layouts. Each layer's real format is resolved the
// way the 4.3.4 client does (Liquid::GetVertexFormat), and its vertex data is rewritten into a layout this client
// reads. Liquid types this client has no record for become the stock type with the same sound bank (water,
// ocean, magma or slime).

#include <cstdint>

struct LiquidCompatClient {
    // True when the client's LiquidType table has a record for this id.
    bool (*hasLiquidType)(uint32_t id);
};

struct LiquidCompatReport {
    uint32_t layers;
    uint32_t objectFormatsResolved;
    uint32_t verticesRewritten;
    uint32_t typesReplaced;
};

// data/size: the MH2O chunk body. Returns false, leaving the rest untouched, when a layer points outside it.
bool LiquidCompatNormalize(uint8_t* data, uint32_t size, const LiquidCompatClient& client, LiquidCompatReport* report);

#endif
