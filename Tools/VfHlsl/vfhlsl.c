/*
 * vfhlsl: compiles one HLSL shader to Direct3D 9 byte code with vkd3d-shader and writes it as a C header, taking
 * the fxc.exe options CoAVolFog's build uses (/T /E /Vn /D /Fh, the rest ignored). Lets the CoAVolFog shaders be
 * built on Linux, where fxc.exe is not available. See README.md next to this file.
 *
 * Not part of the original 12340 client.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vkd3d_shader.h>

static char s_dir[4096];

static bool FileExists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static bool ReadFile(const char* path, struct vkd3d_shader_code* out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* data = malloc(size + 1);
    if (!data || fread(data, 1, size, f) != (size_t)size) {
        fclose(f);
        free(data);
        return false;
    }
    data[size] = 0;
    fclose(f);
    out->code = data;
    out->size = size;
    return true;
}

/* Includes resolve next to the main source, as fxc does for the shaders here. */
static int OpenInclude(const char* filename, bool local, const char* parent, void* context, struct vkd3d_shader_code* out) {
    char path[8192];
    snprintf(path, sizeof(path), "%s/%s", s_dir, filename);
    return ReadFile(path, out) ? VKD3D_OK : VKD3D_ERROR;
}

static void CloseInclude(const struct vkd3d_shader_code* code, void* context) {
    free((void*)code->code);
}

int main(int argc, char** argv) {
    const char *profile = NULL, *entry = "main", *name = NULL, *header = NULL, *source = NULL;
    struct vkd3d_shader_macro macros[64];
    unsigned int macroCount = 0;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (!strcmp(a, "/T") && i + 1 < argc) profile = argv[++i];
        else if (!strcmp(a, "/E") && i + 1 < argc) entry = argv[++i];
        else if (!strcmp(a, "/Vn") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(a, "/Fh") && i + 1 < argc) header = argv[++i];
        else if (!strcmp(a, "/Fc") && i + 1 < argc) i++;
        else if (!strncmp(a, "/D", 2) && macroCount < 64) {
            char* def = strdup(a + 2);
            char* eq = strchr(def, '=');
            if (eq) *eq++ = 0;
            macros[macroCount].name = def;
            macros[macroCount].value = eq ? eq : "1";
            macroCount++;
        } else if (a[0] == '/' && !FileExists(a)) {
            /* /nologo, /O3 and similar: no equivalent needed. An absolute path is the source, not an option. */
        } else source = a;
    }

    /* Lets shaders pick constructs vkd3d-shader handles (see VF_FIXED_LOOP in vf_common.hlsli). */
    if (macroCount < 64) {
        macros[macroCount].name = "VF_VKD3D";
        macros[macroCount].value = "1";
        macroCount++;
    }

    if (!profile || !name || !header || !source) {
        fprintf(stderr, "usage: vfhlsl /T profile /E entry /Vn name [/Dname=value ...] /Fh out.h source.hlsl\n");
        return 2;
    }

    snprintf(s_dir, sizeof(s_dir), "%s", source);
    char* slash = strrchr(s_dir, '/');
    if (slash) *slash = 0; else strcpy(s_dir, ".");

    struct vkd3d_shader_code code;
    if (!ReadFile(source, &code)) {
        fprintf(stderr, "vfhlsl: cannot read %s\n", source);
        return 1;
    }

    struct vkd3d_shader_preprocess_info preprocess = { 0 };
    preprocess.type = VKD3D_SHADER_STRUCTURE_TYPE_PREPROCESS_INFO;
    preprocess.macros = macros;
    preprocess.macro_count = macroCount;
    preprocess.pfn_open_include = OpenInclude;
    preprocess.pfn_close_include = CloseInclude;

    struct vkd3d_shader_hlsl_source_info hlsl = { 0 };
    hlsl.type = VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO;
    hlsl.next = &preprocess;
    hlsl.entry_point = entry;
    hlsl.profile = profile;

    struct vkd3d_shader_compile_info info = { 0 };
    info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    info.next = &hlsl;
    info.source = code;
    info.source_type = VKD3D_SHADER_SOURCE_HLSL;
    // Shader model 4 and 5 profiles (vs_4_0, ps_5_0, ...) come out as DXBC for Direct3D 10/11, older ones as D3D9 bytecode.
    info.target_type = (strstr(profile, "_4_") || strstr(profile, "_5_")) ? VKD3D_SHADER_TARGET_DXBC_TPF : VKD3D_SHADER_TARGET_D3D_BYTECODE;
    info.log_level = VKD3D_SHADER_LOG_WARNING;
    info.source_name = source;

    struct vkd3d_shader_code out = { 0 };
    char* messages = NULL;
    int result = vkd3d_shader_compile(&info, &out, &messages);

    if (messages && *messages) fprintf(stderr, "%s", messages);
    vkd3d_shader_free_messages(messages);

    if (result < 0) {
        fprintf(stderr, "vfhlsl: %s failed to compile (%d)\n", source, result);
        return 1;
    }

    FILE* h = fopen(header, "w");
    if (!h) {
        fprintf(stderr, "vfhlsl: cannot write %s\n", header);
        return 1;
    }

    fprintf(h, "// Generated by Tools/VfHlsl/vfhlsl from %s (%s, entry %s); do not edit.\n", strrchr(source, '/') ? strrchr(source, '/') + 1 : source, profile, entry);
    fprintf(h, "const unsigned char %s[] = {", name);
    const unsigned char* bytes = out.code;
    for (size_t i = 0; i < out.size; i++) fprintf(h, "%s%u,", i % 24 ? " " : "\n    ", bytes[i]);
    fprintf(h, "\n};\n");
    fclose(h);
    vkd3d_shader_free_shader_code(&out);
    return 0;
}
