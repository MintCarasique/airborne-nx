#include "shader_library.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
/*
 * xxHash reads with memcpy (XXH_FORCE_MEMORY_ACCESS 0). With the method it picks for GCC (1) it reads
 * through 64- and 32-bit pointers without may_alias, and GCC may hoist that read above the write of the
 * data being hashed (strict aliasing). That made the texture key read claves[4] before writing it, and
 * the same texture was created several times (tools/comprobar_clave_textura.py checks a built ELF for
 * this). Same fingerprint values; on AArch64, the same LDR.
 */
#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h ya se ha incluido con su implementacion antes de este punto: XXH_FORCE_MEMORY_ACCESS 0 llegaria tarde"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace nfsmw::native {
namespace {
constexpr std::array<uint8_t, 8> kSignature{'N','F','S','S','P','V',0,0};
// AirborneNX: MoH:A has ~10 000 shaders (~140 MB of SPIR-V and containers); NFSMW had about 1000.
constexpr size_t kMaxFile = 512 * 1024 * 1024;
constexpr size_t kMaxShaders = 32768;
constexpr size_t kMaxOriginal = 64 * 1024;
constexpr size_t kMaxSpirv = 4 * 1024 * 1024;

void Require(bool condition, const char* error) {
  if (!condition) throw std::runtime_error(error);
}

uint32_t BE(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 |
         uint32_t(p[2]) << 8 | p[3];
}

struct Reader {
  std::span<const uint8_t> payload;
  size_t position = 0;
  std::span<const uint8_t> Take(size_t n) {
    Require(n <= payload.size() - position, "Paquete de shaders truncado");
    auto r = payload.subspan(position, n);
    position += n;
    return r;
  }
  uint32_t U32() {
    auto p = Take(4);
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
           uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
  }
  uint64_t U64() {
    const uint64_t low = U32();
    return low | uint64_t(U32()) << 32;
  }
};

void U32(std::vector<uint8_t>& d, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) d.push_back(uint8_t(v >> (i * 8)));
}
void U64(std::vector<uint8_t>& d, uint64_t v) {
  U32(d, uint32_t(v)); U32(d, uint32_t(v >> 32));
}

void Validate(Shader& s) {
  Require(s.original.size() >= 24 && s.original.size() <= kMaxOriginal,
         "Longitud del contenedor fuera de limites");
  const uint32_t signature = BE(s.original.data());
  // 2005 (NFSMW) or 2008 (MoH: Airborne). In 2008 the physical part can also hold literal constants after
  // the microcode, so it is only word-aligned.
  const bool de2008 = (signature & ~1u) == 0x102A1100;
  Require((signature & ~1u) == 0x102A0E00 || de2008, "Firma de contenedor desconocida");
  const uint32_t virtuales = BE(s.original.data() + 4);
  const uint32_t physical = BE(s.original.data() + 8);
  Require(virtuales >= (de2008 ? 36u : 24u) && physical && !(physical % (de2008 ? 4 : 12)) &&
             uint64_t(virtuales) + physical == s.original.size(),
         "Longitudes del contenedor incoherentes");
  s.vertices = (signature & 1) != 0;
  s.fingerprint = XXH3_64bits(s.original.data(), s.original.size());
  Require(s.spirv.size() >= 5 && s.spirv.size() <= kMaxSpirv / 4,
         "Longitud SPIR-V fuera de limites");
  Require(s.spirv[0] == 0x07230203 && s.spirv[4] == 0,
         "Cabecera SPIR-V incorrecta");
  // This checks the wrapper; it does not replace spirv-val in the packager.
  bool entry = false;
  for (size_t i = 5; i < s.spirv.size();) {
    const uint32_t word_data = s.spirv[i] >> 16, op = s.spirv[i] & 65535;
    Require(word_data && word_data <= s.spirv.size() - i, "Instruccion SPIR-V truncada");
    if (op == 15) {  // OpEntryPoint: modelo de ejecucion, id, nombre e interfaz.
      Require(!entry && word_data >= 5 && s.spirv[i+1] == (s.vertices ? 0u : 4u) &&
                 s.spirv[i+3] == 0x6e69616d && s.spirv[i+4] == 0,
             "La etapa o la entrada main de SPIR-V no coincide con el contenedor");
      entry = true;
    }
    i += word_data;
  }
  Require(entry, "SPIR-V sin entrada main");
}

bool Lesser(const Shader& a, const Shader& b) {
  if (a.fingerprint != b.fingerprint) return a.fingerprint < b.fingerprint;
  return a.original < b.original;
}
}  // namespace

void ShadersLibrary::Load(std::span<const uint8_t> fname) {
  Require(fname.size() >= 24 && fname.size() <= kMaxFile,
         "Tamano del paquete de shaders fuera de limites");
  Reader l{fname};
  auto signature = l.Take(8);
  Require(std::equal(signature.begin(), signature.end(), kSignature.begin()), "Firma de paquete desconocida");
  Require(l.U32() == 1, "Version de paquete no admitida");
  const uint32_t amount = l.U32();
  Require(amount && amount <= kMaxShaders, "Cantidad de shaders fuera de limites");
  const uint64_t fingerprint = l.U64();
  Require(XXH3_64bits(fname.data() + 24, fname.size() - 24) == fingerprint,
         "Paquete de shaders alterado");
  std::vector<Shader> added_count;
  added_count.reserve(amount);
  for (uint32_t i = 0; i < amount; ++i) {
    const uint32_t original = l.U32(), word_data = l.U32();
    const uint64_t expected_hash = l.U64();
    Require(original >= 24 && original <= kMaxOriginal &&
               word_data >= 5 && word_data <= kMaxSpirv / 4, "Entrada demasiado grande");
    Shader s;
    auto payload = l.Take(original);
    s.original.assign(payload.begin(), payload.end());
    // Reading the whole range first avoids allocations with a truncated file.
    Reader code{l.Take(size_t(word_data) * 4)};
    s.spirv.reserve(word_data);
    for (uint32_t j = 0; j < word_data; ++j) s.spirv.push_back(code.U32());
    Validate(s);
    Require(s.fingerprint == expected_hash, "Huella de contenedor incorrecta");
    if (!added_count.empty()) Require(Lesser(added_count.back(), s), "Entradas repetidas o desordenadas");
    added_count.push_back(std::move(s));
  }
  Require(l.position == fname.size(), "Datos sobrantes en el paquete de shaders");
  shaders_ = std::move(added_count);
}

void ShadersLibrary::Load(const std::filesystem::path& fname) {
  std::ifstream f(fname, std::ios::binary | std::ios::ate);
  Require(bool(f), "No se pudo abrir el paquete de shaders");
  const auto n = f.tellg();
  Require(n >= 24 && n <= std::streamoff(kMaxFile), "Tamano del paquete fuera de limites");
  std::vector<uint8_t> payload(static_cast<size_t>(n));
  f.seekg(0);
  Require(bool(f.read(reinterpret_cast<char*>(payload.data()), payload.size())), "Lectura incompleta del paquete");
  Load(payload);
}

const Shader* ShadersLibrary::Lookup(std::span<const uint8_t> original) const {
  if (original.size() < 24 || original.size() > kMaxOriginal) return nullptr;
  const uint64_t fingerprint = XXH3_64bits(original.data(), original.size());
  auto it = std::lower_bound(shaders_.begin(), shaders_.end(), fingerprint,
      [](const Shader& s, uint64_t h) { return s.fingerprint < h; });
  // A hash collision can never pick another shader: the bytes are compared.
  for (; it != shaders_.end() && it->fingerprint == fingerprint; ++it)
    if (it->original.size() == original.size() &&
        std::equal(original.begin(), original.end(), it->original.begin())) return &*it;
  return nullptr;
}

std::vector<uint8_t> PackShaders(std::vector<Shader> shaders) {
  Require(!shaders.empty() && shaders.size() <= kMaxShaders, "Cantidad de shaders fuera de limites");
  for (auto& s : shaders) Validate(s);
  std::sort(shaders.begin(), shaders.end(), Lesser);
  std::vector<uint8_t> body;
  uint32_t amount = 0;
  const Shader* previous = nullptr;
  for (const auto& s : shaders) {
    if (previous && previous->original == s.original) {
      Require(previous->spirv == s.spirv, "Un contenedor tiene dos traducciones distintas");
      continue;
    }
    const size_t n = 16 + s.original.size() + s.spirv.size() * 4;
    Require(n <= kMaxFile - 24 - body.size(), "Paquete demasiado grande");
    U32(body, uint32_t(s.original.size())); U32(body, uint32_t(s.spirv.size()));
    U64(body, s.fingerprint);
    body.insert(body.end(), s.original.begin(), s.original.end());
    for (uint32_t word_value : s.spirv) U32(body, word_value);
    ++amount;
    previous = &s;
  }
  std::vector<uint8_t> fname(kSignature.begin(), kSignature.end());
  U32(fname, 1); U32(fname, amount);
  U64(fname, XXH3_64bits(body.data(), body.size()));
  fname.insert(fname.end(), body.begin(), body.end());
  return fname;
}
}  // namespace nfsmw::native
