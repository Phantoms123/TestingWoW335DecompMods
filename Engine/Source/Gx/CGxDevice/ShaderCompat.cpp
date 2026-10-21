#include "Engine/Source/Gx/CGxDevice/ShaderCompat.h"
#include <cstring>
#include "ThirdParty/zlib/zlib.h"

static const uint32_t HEADER_SIZE = 7 * 4;
static const uint32_t BLOCK_SIZE = 0x4000;
static const uint32_t PERMUTE_HEADER_SIZE = 32;
static const uint32_t PERMUTE_CODE_SIZE = 28;

static uint32_t Read32(const uint8_t* p) {
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

// True when count 4-byte words starting at offset lie inside size bytes.
static bool WordsFit(uint32_t size, uint32_t offset, uint32_t count) {
    return offset <= size && count <= (size - offset) / 4;
}

bool ShaderCompatFile::Open(const uint8_t* data, uint32_t size) {
    if (size < HEADER_SIZE || Read32(data) != SHADER_COMPAT_MAGIC || Read32(data + 4) != SHADER_COMPAT_VERSION_4X) {
        return false;
    }

    uint32_t permuteOffsets = Read32(data + 8);
    uint32_t permuteCount = Read32(data + 12);
    uint32_t blockOffsets = Read32(data + 16);
    uint32_t blockCount = Read32(data + 20);
    uint32_t blockData = Read32(data + 24);

    // Both tables carry one more entry than their count: the end of the last item.
    if (permuteCount == UINT32_MAX || blockCount == UINT32_MAX || !WordsFit(size, permuteOffsets, permuteCount + 1) ||
        !WordsFit(size, blockOffsets, blockCount + 1) || blockData > size) {
        return false;
    }

    this->m_data = data;
    this->m_size = size;
    this->m_permuteCount = permuteCount;
    this->m_blockCount = blockCount;
    this->m_permuteOffsets = permuteOffsets;
    this->m_blockOffsets = blockOffsets;
    this->m_blockData = blockData;
    return true;
}

uint32_t ShaderCompatFile::PermuteCount() const {
    return this->m_permuteCount;
}

uint32_t ShaderCompatFile::PermuteOffset(uint32_t index) const {
    return Read32(this->m_data + this->m_permuteOffsets + 4 * index);
}

// Copies size bytes of the uncompressed stream, starting at start, into out.
bool ShaderCompatFile::ReadStream(uint32_t start, uint32_t size, uint8_t* out) const {
    uint8_t block[BLOCK_SIZE];

    while (size) {
        uint32_t index = start / BLOCK_SIZE;
        uint32_t within = start % BLOCK_SIZE;

        if (index >= this->m_blockCount) {
            return false;
        }

        uint32_t from = Read32(this->m_data + this->m_blockOffsets + 4 * index);
        uint32_t to = Read32(this->m_data + this->m_blockOffsets + 4 * (index + 1));
        uint32_t available = this->m_size - this->m_blockData;

        if (to < from || to > available) {
            return false;
        }

        const uint8_t* source = this->m_data + this->m_blockData + from;
        uint32_t stored = to - from;
        uLongf length;

        if (stored == BLOCK_SIZE) {
            memcpy(block, source, BLOCK_SIZE);
            length = BLOCK_SIZE;
        } else {
            length = BLOCK_SIZE;

            if (uncompress(block, &length, source, stored) != Z_OK) {
                return false;
            }
        }

        if (within >= length) {
            return false;
        }

        uint32_t take = static_cast<uint32_t>(length) - within;

        if (take > size) {
            take = size;
        }

        memcpy(out, block + within, take);
        out += take;
        start += take;
        size -= take;
    }

    return true;
}

bool ShaderCompatFile::ReadPermute(uint32_t index, ShaderCompatPermute& out) const {
    if (!this->m_data || index >= this->m_permuteCount) {
        return false;
    }

    uint32_t start = this->PermuteOffset(index);
    uint32_t end = this->PermuteOffset(index + 1);

    if (end < start || end - start < PERMUTE_HEADER_SIZE) {
        return false;
    }

    std::vector<uint8_t> bytes(end - start);

    if (!this->ReadStream(start, end - start, bytes.data())) {
        return false;
    }

    uint32_t codeSize = Read32(&bytes[PERMUTE_CODE_SIZE]);

    if (codeSize > bytes.size() - PERMUTE_HEADER_SIZE) {
        return false;
    }

    out.header0 = Read32(&bytes[0]);
    out.header1 = Read32(&bytes[4]);
    memcpy(&out.header2, &bytes[8], 2);
    memcpy(&out.header3, &bytes[10], 2);
    out.code.assign(bytes.begin() + PERMUTE_HEADER_SIZE, bytes.begin() + PERMUTE_HEADER_SIZE + codeSize);
    return true;
}
