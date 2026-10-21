#ifndef ENGINE_SOURCE_GX_CGXDEVICE_SHADERCOMPAT_H
#define ENGINE_SOURCE_GX_CGXDEVICE_SHADERCOMPAT_H

// Reads shader files written for 4.x clients.
//
// Not part of the original 12340 client. 3.3.5a shader files (GXSH 0x10003) store each permutation's code
// one after another. 4.x files (GXSH 0x10004, as the 4.3.4 client's CGxShader::Load and
// CGxShaderPermute::Load read them) store a table of permutation offsets into one stream that is cut into
// 16 KB blocks, each zlib-compressed unless compression did not shrink it. Each permutation begins with a
// 32-byte header whose last word is the size of the code that follows.

#include <cstdint>
#include <vector>

static const uint32_t SHADER_COMPAT_MAGIC = 0x47585348;  // 'GXSH'
static const uint32_t SHADER_COMPAT_VERSION_CLIENT = 0x10003;
static const uint32_t SHADER_COMPAT_VERSION_4X = 0x10004;

// Where features ported from 4.x load that client's shaders from, so they never replace this client's
// shaders of the same name: Shaders\Cata\Pixel\ps_3_0\Name.bls and so on.
#define SHADER_COMPAT_PIXEL_PATH "Shaders\\Cata\\Pixel"
#define SHADER_COMPAT_VERTEX_PATH "Shaders\\Cata\\Vertex"

struct ShaderCompatPermute {
    // The header words this client's CGxShader keeps (unk_003C, unk_0040, unk_0044, unk_0046).
    uint32_t header0;
    uint32_t header1;
    uint16_t header2;
    uint16_t header3;
    // Shader code as the device creates it (D3D9 bytecode in a ps_x_x / vs_x_x file). Empty when the
    // permutation has no code for this profile.
    std::vector<uint8_t> code;
};

class ShaderCompatFile {
    public:
    // data/size: a whole .bls file. Returns false when it is not a 4.x file or its tables point outside it.
    bool Open(const uint8_t* data, uint32_t size);
    uint32_t PermuteCount() const;
    // Returns false, leaving out untouched, when the permutation's data is damaged.
    bool ReadPermute(uint32_t index, ShaderCompatPermute& out) const;

    private:
    const uint8_t* m_data = nullptr;
    uint32_t m_size = 0;
    uint32_t m_permuteCount = 0;
    uint32_t m_blockCount = 0;
    uint32_t m_permuteOffsets = 0;
    uint32_t m_blockOffsets = 0;
    uint32_t m_blockData = 0;

    uint32_t PermuteOffset(uint32_t index) const;
    bool ReadStream(uint32_t start, uint32_t size, uint8_t* out) const;
};

#endif
