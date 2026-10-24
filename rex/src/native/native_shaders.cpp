// nfsmw - native renderer, part C5a (see native_shaders.h).
//
// 2005 container format: see Leer(). All fields are big-endian.

#include "native_shaders.h"

#include "shader_library.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include <cstdio>
#include <filesystem>

#include <algorithm>
#include <exception>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>

/*
 * xxHash reads with memcpy (XXH_FORCE_MEMORY_ACCESS 0). With the method it picks for GCC (1) it reads through
 * 64- and 32-bit pointers without may_alias, and GCC may move that read ahead of the store of the data being
 * hashed (strict aliasing). With that, the texture key read claves[4] before writing it and the same texture was
 * created several times (see docs/toolchain.md). Same fingerprint values; on AArch64, the same LDR.
 */
#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h ya se ha incluido con su implementacion antes de este punto: XXH_FORCE_MEMORY_ACCESS 0 llegaria tarde"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

REXCVAR_DEFINE_BOOL(airborne_dump_unknown_shaders, false, "AirborneNX",
                    "Native renderer (development): write the microcode of every shader the library does not "
                    "have to unknown_shaders/<hash>_<vs|ps>.bin next to the executable, for "
                    "tools/wrap_unknown_shaders.py");

namespace nfsmw::native_renderer {
namespace {

// AirborneNX: shaders that come without a container (built at run time by the XDK or the game) are dumped
// as raw microcode: a "VSMC"/"PSMC" tag, the word count, then the words big-endian as the GPU reads them.
void DumpUnknownShader(bool vertex, uint64_t hash, std::span<const uint32_t> microcode) {
  std::error_code ec;
  const auto folder = rex::filesystem::GetExecutableFolder() / "unknown_shaders";
  std::filesystem::create_directories(folder, ec);
  const auto path = folder / fmt::format("{:016X}_{}.bin", hash, vertex ? "vs" : "ps");
  if (std::filesystem::exists(path, ec)) {
    return;
  }
  FILE* file = std::fopen(path.string().c_str(), "wb");
  if (!file) {
    REXLOG_WARN("[nativo] C5a: cannot write {}", path.string());
    return;
  }
  const uint32_t header[2] = {__builtin_bswap32(vertex ? 0x56534D43u : 0x50534D43u),
                              __builtin_bswap32(uint32_t(microcode.size()))};
  std::fwrite(header, sizeof(header), 1, file);
  for (const uint32_t word : microcode) {
    const uint32_t be = __builtin_bswap32(word);
    std::fwrite(&be, sizeof(be), 1, file);
  }
  std::fclose(file);
}

// Bits of a vertex fetch instruction that D3D does not touch
// (VertexFetchInstruction in XenosRecomp shader_code.h): opcode, source and
// destination registers and predicate. The rest (fetch constant, format, stride,
// offset and numeric modes) comes from the declaration.
// AirborneNX: MoH:A's XDK also rewrites the destination swizzle (a float3 position
// gets w = 1: 0x688 -> 0xA88, with format 0x39 = 32_32_32_FLOAT), so it is not kept
// either (NFSMW kept it: 0x80000FFF).
// AirborneNX: and the destination register too: the XDK can permute the registers of a group of fetches
// (a particle VS arrives with r4/r6/r5 where the container has r6/r5/r4). The renderer finds each element's
// fetch by register in the patched microcode, not by position.
constexpr uint32_t kKeepsFetch[3] = {0x00000FFF, 0x80000000, 0x80000000};

constexpr uint32_t kMaxWarnings = 48;

constexpr const char* kUses[] = {"posicion", "peso",     "indices",    "normal",
                                 "tam_punto", "texcoord", "tangente",   "binormal",
                                 "teselado",  "posicion_t", "color",    "niebla",
                                 "profundidad", "muestra", "uso14",     "uso15"};

struct Container {
  const std::vector<uint8_t>& o;

  bool Hay(size_t position, size_t bytes) const {
    return position <= o.size() && bytes <= o.size() - position;
  }
  uint32_t U32(size_t p) const {
    return uint32_t(o[p]) << 24 | uint32_t(o[p + 1]) << 16 | uint32_t(o[p + 2]) << 8 | o[p + 3];
  }
  uint16_t U16(size_t p) const { return uint16_t(uint32_t(o[p]) << 8 | o[p + 1]); }
};

// nfsmw_nativo_sombra_minimo. The zero-terminated string starting at `posicion` is `nombre`.
bool NameIs(const Container& c, size_t position, const char* name) {
  const size_t n = std::strlen(name);
  return c.Hay(position, n + 1) && std::memcmp(c.o.data() + position, name, n) == 0 && c.o[position + n] == 0;
}

// Reads what is needed from the 2005 container, which is not the one in XenosRecomp
// shader.h (that is the 2008 one): 24-byte header with signature, virtual part,
// physical part, definitions (+12), CTAB (+16) and shader header (+20). The
// microcode is the whole physical part. Reference: docs/shaders.md and
// shaders/nfsmw_contenedor.h (Convertir2005). Returns the reason if it cannot be
// read.
//
// AirborneNX: MoH: Airborne's containers are the 2008 layout (signature 0x102A11xx, the one in XenosRecomp
// shader.h): 36-byte header, CTAB at +16 as in 2005, shader header at +24, and the microcode is the range
// [physicalOffset, physicalOffset + size) of the physical part (literal constants can follow it). The
// vertex element list starts at +0x24 of the shader header instead of +0x28.
const char* Read(const nfsmw::native::Shader& shader, ShaderEntry& e) {
  const Container c{shader.original};
  if (!c.Hay(0, 24)) return "contenedor demasiado corto";
  const bool de2008 = (c.U32(0) & 0xFFFFFF00u) == 0x102A1100u;
  if (de2008 && !c.Hay(0, 36)) return "contenedor demasiado corto";
  const uint32_t virtuales = c.U32(4);
  const uint32_t physical = c.U32(8);
  const uint32_t table = c.U32(16);
  const uint32_t hdr = c.U32(de2008 ? 24 : 20);
  const size_t item_list = de2008 ? 36 : 40;
  if (!physical || (physical % 4) || !c.Hay(virtuales, physical)) {
    return "microcodigo fuera del contenedor";
  }
  if (hdr >= virtuales || size_t(hdr) + (shader.vertices ? item_list : 32) > virtuales) {
    return "cabecera del shader fuera de la parte virtual";
  }
  uint32_t code_start = 0, code_bytes = physical;
  if (de2008) {
    code_start = c.U32(hdr);
    code_bytes = c.U32(hdr + 4);
    if (!code_bytes || (code_start % 4) || (code_bytes % 4) || code_start > physical ||
        code_bytes > physical - code_start) {
      return "microcodigo fuera de la parte fisica";
    }
  }

  e.vertices = shader.vertices;
  e.ucode.resize(code_bytes / 4);
  for (size_t i = 0; i < e.ucode.size(); ++i) {
    e.ucode[i] = c.U32(size_t(virtuales) + code_start + i * 4);
  }

  if (e.vertices) {
    // List at +0x28 (2005) or +0x24 (2008), after skipping the words at +0x18; +0x1C = element count.
    const uint32_t previous_items = c.U32(hdr + 24);
    const uint32_t amount = c.U32(hdr + 28);
    const size_t begin_pos = size_t(hdr) + item_list + size_t(previous_items) * 4;
    if (amount > 64 || previous_items > 1024 || begin_pos + size_t(amount) * 4 > virtuales) {
      return "elementos de vertices fuera de la parte virtual";
    }
    for (uint32_t i = 0; i < amount; ++i) {
      const uint32_t value = c.U32(begin_pos + size_t(i) * 4);
      VertexElement element;
      element.instruction = uint16_t(value & 0xFFF);
      element.use = uint8_t((value >> 12) & 0xF);
      element.use_index = uint8_t((value >> 16) & 0xF);
      if ((size_t(element.instruction) + 1) * 3 > e.ucode.size()) {
        return "elemento de vertices apunta fuera del microcodigo";
      }
      element.swizzle = uint16_t(e.ucode[size_t(element.instruction) * 3 + 1] & 0xFFF);
      element.reg_index = uint8_t((e.ucode[size_t(element.instruction) * 3] >> 12) & 0x3F);
      e.elements.push_back(element);
      for (size_t j = 0; j < 3; ++j) {
        e.ucode[size_t(element.instruction) * 3 + j] &= kKeepsFetch[j];
      }
    }
  } else {
    if (!c.Hay(hdr + 24, 8)) return "cabecera de pixel shader corta";
    e.outputs = c.U32(hdr + 28);
  }

  // Constant table: only the samplers (register and type).
  if (!table || !c.Hay(table + 4, 28)) return "sin tabla de constantes";
  const size_t base = size_t(table) + 4;
  const uint32_t constantes = c.U32(base + 12);
  const uint32_t info = c.U32(base + 16);
  if (constantes > 1024 || !c.Hay(base + info, size_t(constantes) * 20)) {
    return "tabla de constantes fuera del contenedor";
  }
  // Float registers the SPIR-V reads: the highest in the table, or up to the end of the
  // buffer if there is an array with relative indexing (XenosRecomp shader_recompiler.cpp:
  // 1172-1183: tailCount = 256 in VS and 224 in PS).
  uint32_t float_regs = 0;
  for (uint32_t i = 0; i < constantes; ++i) {
    const size_t p = base + info + size_t(i) * 20;
    if (c.U16(p + 4) == 2) {  // RegisterSet::Float4
      const uint32_t index = c.U16(p + 6);
      const uint32_t how_many_items = c.U16(p + 8);
      float_regs = std::max(float_regs, how_many_items > 1 ? (e.vertices ? 256u : 224u)
                                                              : index + 1);
    }
  }
  e.constantes_bytes = std::min<uint32_t>(std::max<uint32_t>(float_regs, 1), 256) * 16;
  for (uint32_t i = 0; i < constantes; ++i) {
    const size_t p = base + info + size_t(i) * 20;
    if (c.U16(p + 4) != 3) continue;  // RegisterSet::Sampler
    SamplerShader sampler;
    sampler.reg_index = c.U16(p + 6);
    const uint32_t type = c.U32(p + 12);
    sampler.type = c.Hay(base + type, 4) ? c.U16(base + type + 2) : 0;
    // nfsmw_nativo_sombra_minimo. The name is at that distance from the start of the table, like the type
    // (XenosRecomp shader_recompiler.cpp: constantTableData + constantInfo->name).
    sampler.shadows_map = NameIs(c, base + size_t(c.U32(p)), "SHADOWMAP_SAMPLER");
    e.samplers.push_back(sampler);
  }

  // AirborneNX: texture fetches on registers the constant table does not declare. tools/shaders_native
  // declares those samplers itself, so they have to be bound too. The control flow comes first, as pairs of
  // 48-bit instructions in 3 words, up to the lowest exec address; each exec clause marks its fetch
  // instructions with bit 0 of each 2-bit sequence entry.
  e.table_samplers = uint32_t(e.samplers.size());
  if (!e.vertices) {
    const size_t instructions = e.ucode.size() / 3;
    size_t limit = instructions;
    for (size_t i = 0; i < limit && i < instructions; ++i) {
      const uint32_t a = e.ucode[i * 3], b = e.ucode[i * 3 + 1], w = e.ucode[i * 3 + 2];
      const uint64_t cf[2] = {uint64_t(a) | uint64_t(b & 0xFFFF) << 32, uint64_t(b >> 16) | uint64_t(w) << 16};
      for (const uint64_t word_value : cf) {
        const uint32_t op = uint32_t(word_value >> 44) & 0xF;
        if (!((op >= 1 && op <= 6) || op == 13 || op == 14)) {
          continue;
        }
        const uint32_t address = uint32_t(word_value) & 0xFFF, amount = uint32_t(word_value >> 12) & 0x7;
        const uint32_t sequence = uint32_t(word_value >> 16) & 0xFFF;
        if (address) {
          limit = std::min<size_t>(limit, address);
        }
        for (uint32_t j = 0; j < amount; ++j) {
          const size_t k = size_t(address) + j;
          if (!((sequence >> (j * 2)) & 0x1) || (k + 1) * 3 > e.ucode.size()) {
            continue;
          }
          const uint32_t w0 = e.ucode[k * 3], w2 = e.ucode[k * 3 + 2];
          const uint32_t opcode = w0 & 0x1F;
          if (opcode != 1 && (opcode < 16 || opcode > 19)) {
            continue;
          }
          const uint16_t reg_index = uint16_t((w0 >> 20) & 0x1F);
          if (std::any_of(e.samplers.begin(), e.samplers.end(),
                          [&](const SamplerShader& s) { return s.reg_index == reg_index; })) {
            continue;
          }
          const uint32_t dimension = (w2 >> 14) & 0x3;  // 0 1D, 1 2D, 2 3D, 3 cube
          SamplerShader sampler;
          sampler.reg_index = reg_index;
          sampler.type = dimension == 2 ? 13 : dimension == 3 ? 14 : 12;
          e.samplers.push_back(sampler);
        }
      }
    }
  }

  e.fingerprint = XXH3_64bits(e.ucode.data(), e.ucode.size() * sizeof(uint32_t));
  return nullptr;
}

struct RawKey {
  uint64_t fingerprint;
  uint32_t word_data;
  bool vertices;
  bool operator==(const RawKey&) const = default;
};

struct RawKeyHash {
  size_t operator()(const RawKey& c) const {
    return size_t(c.fingerprint ^ (uint64_t(c.word_data) << 1) ^ uint64_t(c.vertices));
  }
};

}  // namespace

struct NativeShaders::Impl {
  nfsmw::native::ShadersLibrary library;
  std::vector<ShaderEntry> entries;
  std::unordered_map<const nfsmw::native::Shader*, uint32_t> por_shader;
  // (vertices, palabras) -> entradas candidatas.
  std::map<std::pair<bool, uint32_t>, std::vector<uint32_t>> candidates;
  std::unordered_map<RawKey, const ShaderEntry*, RawKeyHash> cache;
  std::vector<uint32_t> temporal;
  ShaderStats stats;
  uint32_t warnings = 0;
  bool loaded = false;
};

NativeShaders::NativeShaders() : payload_(std::make_unique<Impl>()) {}
NativeShaders::~NativeShaders() = default;

bool NativeShaders::loaded() const {
  return payload_->loaded;
}

bool FetchCoherentes(const ShaderEntry& vs, std::span<const uint32_t> patched) {
  if (!vs.vertices || patched.size() != vs.ucode.size()) {
    return false;
  }
  for (const VertexElement& element : vs.elements) {
    const uint32_t reg_index = element.reg_index;
    bool found = false;
    for (const VertexElement& other_one : vs.elements) {
      const uint32_t d0 = patched[size_t(other_one.instruction) * 3];
      if (((d0 >> 12) & 0x3F) == reg_index && (d0 & 0x1F) == 0) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

// nfsmw_nativo_sombra_minimo. The library has tfetch2DSombraMin if the pixel shader's SPIR-V contains the
// NFSMW_MARCA_SOMBRA_MINIMO constant from shader_common.h (a 32-bit OpConstant): nothing else uses it.
constexpr uint32_t kShadowMinMark = 0x5E3B1A84u;
static bool HasShadowMinMark(const std::vector<uint32_t>& spirv) {
  if (spirv.size() < 5 || spirv[0] != 0x07230203u) {
    return false;
  }
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t word_data = spirv[i] >> 16;
    if (word_data == 0 || i + word_data > spirv.size()) {
      return false;
    }
    if ((spirv[i] & 0xFFFF) == 43 && word_data == 4 && spirv[i + 3] == kShadowMinMark) {
      return true;
    }
    i += word_data;
  }
  return false;
}

// OpKill, OpTerminateInvocation and OpDemoteToHelperInvocation in the module. If the SPIR-V cannot be walked
// it returns 2, the conservative answer (the fragment stage is kept).
static uint32_t CountKills(const std::vector<uint32_t>& spirv) {
  if (spirv.size() < 5 || spirv[0] != 0x07230203u) {
    return 2;
  }
  uint32_t kills = 0;
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t word_data = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    if (word_data == 0 || i + word_data > spirv.size()) {
      return 2;
    }
    if (code == 252 || code == 4416 || code == 5380) {
      ++kills;
    }
    i += word_data;
  }
  return kills;
}

bool NativeShaders::Load(const std::filesystem::path& fname) {
  Impl& d = *payload_;
  try {
    d.library.Load(fname);
  } catch (const std::exception& e) {
    REXLOG_WARN("[nativo] C5a: biblioteca de shaders no disponible ({}): {}", fname.string(),
                e.what());
    return false;
  }
  const auto& shaders = d.library.shaders();
  uint32_t con_kill = 0, without_kill = 0;
  uint32_t with_shadows_map = 0, with_min = 0;  // nfsmw_nativo_sombra_minimo
  d.entries.clear();
  d.entries.reserve(shaders.size());
  uint32_t vertex = 0, pixel = 0;
  for (uint32_t i = 0; i < shaders.size(); ++i) {
    ShaderEntry e;
    e.shader = &shaders[i];
    e.number = i;
    if (const char* reason = Read(shaders[i], e)) {
      REXLOG_WARN("[nativo] C5a: contenedor {} de la biblioteca ignorado: {}", i, reason);
      continue;
    }
    if (!e.vertices) {
      e.kills = CountKills(shaders[i].spirv);
      e.discards = e.kills != 1;                // 1 = only the alpha test one
      (e.discards ? con_kill : without_kill) += 1;
      // nfsmw_nativo_sombra_minimo. Whether its SPIR-V can take the shadow map minimum.
      e.shadow_min = HasShadowMinMark(shaders[i].spirv);
      for (const SamplerShader& s : e.samplers) {
        if (s.shadows_map) {
          ++with_shadows_map;
          with_min += e.shadow_min ? 1 : 0;
          break;
        }
      }
    }
    (e.vertices ? vertex : pixel) += 1;
    d.entries.push_back(std::move(e));
  }
  d.candidates.clear();
  d.por_shader.clear();
  std::map<std::tuple<bool, uint32_t, uint64_t>, std::vector<uint32_t>> groups;
  for (uint32_t i = 0; i < d.entries.size(); ++i) {
    const ShaderEntry& e = d.entries[i];
    const uint32_t word_data = uint32_t(e.ucode.size());
    d.candidates[{e.vertices, word_data}].push_back(i);
    d.por_shader[e.shader] = i;
    groups[{e.vertices, word_data, e.fingerprint}].push_back(i);
  }
  uint32_t repeated = 0, distinct_repeated = 0;
  for (const auto& [key, members] : groups) {
    if (members.size() < 2) continue;
    ++repeated;
    const auto& spirv = d.entries[members[0]].shader->spirv;
    for (uint32_t m : members) {
      if (d.entries[m].shader->spirv != spirv) {
        ++distinct_repeated;
        break;
      }
    }
  }
  d.cache.clear();
  d.loaded = !d.entries.empty();
  REXLOG_INFO("[nativo] C5a: biblioteca con {} shaders ({} vertex, {} pixel); {} grupos con el "
              "mismo microcodigo, {} de ellos con SPIR-V distinto",
              d.entries.size(), vertex, pixel, repeated, distinct_repeated);
  REXLOG_INFO("[nativo] C5a: pixel shaders que pueden descartar pixeles {} de {} (el resto solo llevan el "
              "kill de la prueba de alfa: sin color que escribir, su etapa se puede quitar)",
              con_kill, con_kill + without_kill);
  REXLOG_INFO("[nativo] C5a: sombra por minimo (build 184): {} pixel shaders muestrean el mapa de sombras "
              "(SHADOWMAP_SAMPLER) y {} traen tfetch2DSombraMin{}",
              with_shadows_map, with_min,
              with_min && with_min == with_shadows_map
                  ? " (biblioteca con el minimo)"
                  : " (sin el minimo: el mapa de sombras se sigue copiando como siempre)");
  return d.loaded;
}

const ShaderEntry* NativeShaders::Identify(bool vertices,
                                                 std::span<const uint32_t> ucode) {
  Impl& d = *payload_;
  ++d.stats.loads;
  const RawKey key{XXH3_64bits(ucode.data(), ucode.size_bytes()),
                         uint32_t(ucode.size()), vertices};
  if (auto it = d.cache.find(key); it != d.cache.end()) {
    return it->second;
  }
  ++d.stats.distinct_items;

  const ShaderEntry* chosen = nullptr;
  uint32_t matches = 0;
  if (auto c = d.candidates.find({vertices, uint32_t(ucode.size())});
      c != d.candidates.end()) {
    for (uint32_t index : c->second) {
      const ShaderEntry& e = d.entries[index];
      d.temporal.assign(ucode.begin(), ucode.end());
      for (const VertexElement& element : e.elements) {
        for (size_t j = 0; j < 3; ++j) {
          d.temporal[size_t(element.instruction) * 3 + j] &= kKeepsFetch[j];
        }
      }
      if (XXH3_64bits(d.temporal.data(), d.temporal.size() * sizeof(uint32_t)) != e.fingerprint ||
          d.temporal != e.ucode) {
        continue;
      }
      // AirborneNX: the same microcode can come in containers whose constant tables declare fewer samplers
      // (UE3 material variants); XenosRecomp drops the texture fetches it has no sampler for. Prefer the
      // container that declares the most samplers, then the most constants.
      if (!chosen || e.table_samplers > chosen->table_samplers ||
          (e.table_samplers == chosen->table_samplers && e.constantes_bytes > chosen->constantes_bytes)) {
        chosen = &e;
      }
      ++matches;
    }
  }

  // AirborneNX: when a VS is bound with a PS that does not read some of its interpolators, MoH:A's XDK turns
  // those export instructions into no-ops (word 0 loses the export bit and both write masks: C80F8002 ->
  // C8000000; the source words change too). A candidate whose only differences are such instructions is the
  // same shader: its SPIR-V still writes the interpolator, which the PS ignores.
  if (!chosen && vertices) {
    if (auto c = d.candidates.find({vertices, uint32_t(ucode.size())}); c != d.candidates.end()) {
      for (uint32_t index : c->second) {
        const ShaderEntry& e = d.entries[index];
        bool is_equal = true;
        bool any = false;
        for (size_t k = 0; is_equal && k * 3 + 2 < ucode.size(); ++k) {
          bool distinct_alt = false;
          for (size_t j = 0; j < 3; ++j) {
            uint32_t inbound = ucode[k * 3 + j];
            for (const VertexElement& element : e.elements) {
              if (size_t(element.instruction) == k) inbound &= kKeepsFetch[j];
            }
            distinct_alt |= inbound != e.ucode[k * 3 + j];
          }
          if (!distinct_alt) continue;
          // Only from instruction 1 on (instruction 0 holds the control flow) and only the export pattern.
          const uint32_t incoming0 = ucode[k * 3], original0 = e.ucode[k * 3];
          is_equal = k > 0 && (original0 & 0x8000u) && !(incoming0 & 0x00FF8000u) &&
                  (incoming0 & 0xFC000000u) == (original0 & 0xFC000000u);
          any = true;
        }
        if (is_equal && any) {
          chosen = &e;
          ++matches;
          ++d.stats.overridden_exports;
          break;
        }
      }
    }
  }

  // Vertex shaders arrive patched: a mismatch is normal.
  // AirborneNX: vertex shaders too (MoH:A patches them only at creation; a miss is a missing shader).
  const bool warn = d.warnings < kMaxWarnings;
  if (chosen) {
    ++d.stats.identified;
    if (matches > 1) {
      ++d.stats.ambiguos;
    }
    // AirborneNX: one line per shader the ring identifies (52 distinct ones at the title screen).
    if (d.stats.distinct_items <= 256) {
      std::string used_items;
      for (const SamplerShader& sampler : chosen->samplers) {
        used_items += fmt::format(" s{}:{}", sampler.reg_index, sampler.type);
      }
      REXLOG_INFO("[nativo] C5a: {} n{} identificado ({} palabras, {} coincidencias, {} bytes de constantes, "
                  "samplers:{}; contenedor {:016X})",
                  vertices ? "VS" : "PS", chosen->number, ucode.size(), matches,
                  chosen->constantes_bytes, used_items.empty() ? " ninguno" : used_items, chosen->shader->fingerprint);
    }
    if (warn && vertices) {
      // What D3D has patched in each fetch: it becomes the vertex input.
      ++d.warnings;
      std::string detail;
      for (const VertexElement& element : chosen->elements) {
        const size_t p = size_t(element.instruction) * 3;
        const uint32_t d0 = ucode[p], d1 = ucode[p + 1], d2 = ucode[p + 2];
        const uint32_t fetch = ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 0x3);
        const int32_t offset = int32_t(d2 << 1) >> 9;
        detail += fmt::format(" {}{}:f{}/fmt{}/z{}/o{}{}", kUses[element.use & 0xF],
                               element.use_index, fetch, (d1 >> 16) & 0x3F, d2 & 0xFF, offset,
                               ((d1 >> 30) & 0x1) ? "/mini" : "");
      }
      REXLOG_INFO("[nativo] C5a: VS n{} ({} palabras, {} coincidencias):{}", chosen->number,
                  ucode.size(), matches, detail);
    }
  } else {
    ++d.stats.without_identify;
    if (REXCVAR_GET(airborne_dump_unknown_shaders)) {
      DumpUnknownShader(vertices, key.fingerprint, ucode);
    }
    if (warn) {
      ++d.warnings;
      const auto c = d.candidates.find({vertices, uint32_t(ucode.size())});
      REXLOG_WARN("[nativo] C5a: {} shader sin identificar: {} palabras, huella {:016X} "
                  "({} contenedores de ese tipo y longitud)",
                  vertices ? "vertex" : "pixel", ucode.size(), key.fingerprint,
                  c != d.candidates.end() ? c->second.size() : 0);
      // AirborneNX: the whole microcode, to look for shaders that come without a container (XDK internals).
      if (ucode.size() <= 128) {
        std::string words;
        for (const uint32_t w : ucode) words += fmt::format(" {:08X}", w);
        REXLOG_WARN("[nativo] C5a:   microcode:{}", words);
      }
      // Diagnostics: the closest container of the same length (fewest differing words after the fetch
      // mask) and which words change (incoming / container).
      const ShaderEntry* nearest = nullptr;
      uint32_t minus = UINT32_MAX;
      for (size_t k = 0; c != d.candidates.end() && k < c->second.size(); ++k) {
        const ShaderEntry& e = d.entries[c->second[k]];
        uint32_t how_many = 0;
        for (size_t i = 0; i < ucode.size(); ++i) {
          uint32_t inbound = ucode[i];
          for (const VertexElement& element : e.elements) {
            const size_t p = size_t(element.instruction) * 3;
            if (i >= p && i < p + 3) inbound &= kKeepsFetch[i - p];
          }
          how_many += inbound != e.ucode[i];
        }
        if (how_many < minus) {
          minus = how_many;
          nearest = &e;
        }
      }
      if (nearest) {
        std::string differences;
        uint32_t shown = 0;
        for (size_t i = 0; i < ucode.size() && shown < 32; ++i) {
          uint32_t inbound = ucode[i];
          for (const VertexElement& element : nearest->elements) {
            const size_t p = size_t(element.instruction) * 3;
            if (i >= p && i < p + 3) inbound &= kKeepsFetch[i - p];
          }
          if (inbound != nearest->ucode[i]) {
            ++shown;
            differences += fmt::format(" {}:{:08X}/{:08X}", i, ucode[i], nearest->ucode[i]);
          }
        }
        REXLOG_WARN("[nativo] C5a:   el mas cercano n{}: {} palabras distintas:{}", nearest->number, minus,
                    differences);
      }
    }
  }
  d.cache.emplace(key, chosen);
  return chosen;
}

const ShaderEntry* NativeShaders::IdentifyContainer(
    std::span<const uint8_t> container) const {
  const Impl& d = *payload_;
  if (!d.loaded) {
    return nullptr;
  }
  const nfsmw::native::Shader* shader = d.library.Lookup(container);
  if (!shader) {
    return nullptr;
  }
  const auto it = d.por_shader.find(shader);
  return it != d.por_shader.end() ? &d.entries[it->second] : nullptr;
}

// Entries are in library order (Cargar), with gaps where a container was skipped.
const ShaderEntry* NativeShaders::ByNumber(uint32_t number) const {
  const Impl& d = *payload_;
  if (!d.loaded) {
    return nullptr;
  }
  const auto it = std::lower_bound(d.entries.begin(), d.entries.end(), number,
                                   [](const ShaderEntry& e, uint32_t n) { return e.number < n; });
  return it != d.entries.end() && it->number == number ? &*it : nullptr;
}

const char* UseName(uint8_t use) {
  return kUses[use & 0xF];
}

ShaderStats NativeShaders::Stats() const {
  return payload_->stats;
}

}  // namespace nfsmw::nativo
