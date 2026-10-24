// nfsmw - native renderer, C3: sub-allocation pool for textures.
// The rationale, the measurements and the safety invariant are in native_texture_pool.h.

#include "native_texture_pool.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

#include <algorithm>
#include <string>

REXCVAR_DEFINE_BOOL(nfsmw_nativo_texturas_pool, true, "NFSMW",
                    "Renderizador nativo (22/09): las texturas toman trozos de bloques grandes de memoria "
                    "en vez de pedir una reserva dedicada cada una. En Horizon cada reserva dedicada cuesta "
                    "~1,9 ms de CPU (un nvMapCreate, DOS reservas de direccion y DOS mapeos), y la imagen no "
                    "cambia. Apagarlo vuelve a la reserva por textura")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(nfsmw_nativo_texturas_pool_slab_mb, 32, "NFSMW",
                     "Renderizador nativo: MB de cada bloque grande del pool de texturas. Mas grande = menos "
                     "reservas al sistema, pero cada bloque nuevo cuesta un memset de ese tamano")
    .range(4, 256)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace nfsmw::native_renderer {
namespace {

uint32_t AlignUp(uint32_t value, uint32_t step) {
  if (step <= 1) {
    return value;
  }
  return ((value + step - 1) / step) * step;
}

bool OccupiedBit(const std::vector<uint64_t>& map_table, uint32_t i) {
  return ((map_table[i >> 6] >> (i & 63)) & 1ull) != 0ull;
}

// Returns true if [inicio, inicio+n) is entirely free. Otherwise it stores in `primera_ocupada`
// the first used unit it found, so the search can jump past it.
bool FreeRange(const std::vector<uint64_t>& map_table, uint32_t start, uint32_t n,
                uint32_t& first_occupied) {
  for (uint32_t i = start; i < start + n; ++i) {
    if (OccupiedBit(map_table, i)) {
      first_occupied = i;
      return false;
    }
  }
  return true;
}

void MarkRange(std::vector<uint64_t>& map_table, uint32_t start, uint32_t n, bool busy) {
  for (uint32_t i = start; i < start + n; ++i) {
    if (busy) {
      map_table[i >> 6] |= 1ull << (i & 63);
    } else {
      map_table[i >> 6] &= ~(1ull << (i & 63));
    }
  }
}

/*
 * First fit, without a rover.
 *
 * With 32 MB blocks there are 512 units, so a full pass is at most a few hundred bit checks,
 * and jumping past the first used unit keeps it linear. At ten or twenty textures per second
 * this does not show up in measurements; a rover would only add a second pass and more places
 * to get it wrong.
 */
bool FindSlot(const std::vector<uint64_t>& map_table, uint32_t total_units, uint32_t n,
                 uint32_t step, uint32_t& start_out) {
  if (n == 0 || n > total_units) {
    return false;
  }
  uint32_t start = 0;
  while (start + n <= total_units) {
    uint32_t occupied = 0;
    if (FreeRange(map_table, start, n, occupied)) {
      start_out = start;
      return true;
    }
    start = AlignUp(occupied + 1, step);
  }
  return false;
}

// The largest contiguous free range, in units. It is the fragmentation measure in the report.
uint32_t SlotGreater(const std::vector<uint64_t>& map_table, uint32_t total_units) {
  uint32_t best = 0;
  uint32_t current = 0;
  for (uint32_t i = 0; i < total_units; ++i) {
    if (OccupiedBit(map_table, i)) {
      current = 0;
    } else {
      ++current;
      best = std::max(best, current);
    }
  }
  return best;
}

}  // namespace

TexturePool::~TexturePool() { Finish(); }

bool TexturePool::PickMemoryType(uint32_t& type_out, uint64_t& view_alignment_out) const {
  /*
   * A sample image with the same usage and tiling as the real textures, only to ask the
   * driver which memory types it accepts and with what alignment. It is destroyed right
   * away; it does not allocate memory.
   */
  const auto& dfn = vk_device_->functions();
  VkImageCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = VK_FORMAT_R8G8B8A8_UNORM;
  info.extent = {256, 256, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImage sample = VK_NULL_HANDLE;
  if (dfn.vkCreateImage(device_, &info, nullptr, &sample) != VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements requirements{};
  dfn.vkGetImageMemoryRequirements(device_, sample, &requirements);
  dfn.vkDestroyImage(device_, sample, nullptr);

  // The same criterion CreateDedicatedAllocationImage uses for textures, so the slab lands
  // in the same memory type the dedicated allocation would.
  const uint32_t type = rex::ui::vulkan::util::ChooseMemoryType(
      vk_device_->memory_types(), requirements.memoryTypeBits,
      rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal);
  if (type == UINT32_MAX) {
    return false;
  }
  type_out = type;
  view_alignment_out = requirements.alignment;
  return true;
}

bool TexturePool::Start(const rex::ui::vulkan::VulkanDevice* dev, int32_t mb_cache_max) {
  if (!REXCVAR_GET(nfsmw_nativo_texturas_pool)) {
    REXLOG_INFO("[nativo] C3: pool de texturas (nfsmw_nativo_texturas_pool) = no; cada textura pedira su "
                "reserva dedicada, como antes del 22/09");
    return false;
  }
  if (dev == nullptr) {
    return false;
  }
  vk_device_ = dev;
  device_ = dev->device();

  uint64_t view_alignment = 0;
  if (!PickMemoryType(memory_type_, view_alignment)) {
    REXLOG_WARN("[nativo] C3: pool de texturas: no hay un tipo de memoria local valido; se sigue con la "
                "reserva dedicada por textura");
    vk_device_ = nullptr;
    device_ = VK_NULL_HANDLE;
    return false;
  }

  const int32_t slab_mb = std::max<int32_t>(REXCVAR_GET(nfsmw_nativo_texturas_pool_slab_mb), 4);
  slab_bytes_ = uint64_t(slab_mb) << 20;
  units_slab_ = uint32_t(slab_bytes_ / kPoolUnitBytes);

  /*
   * How much to prewarm and how far to grow.
   *
   * At startup, half the cache limit (clamped between 64 and 256 MB): the cache currently
   * stays at about 159 MB of 384, so with that the pool almost never needs to grow during a
   * race. The cap is one slab above the cache limit to leave room for fragmentation.
   */
  const int32_t mb_max = mb_cache_max > 0 ? mb_cache_max : 384;
  const int32_t initial_mb = std::clamp<int32_t>(mb_max / 2, 64, 256);
  const uint64_t bytes_cap = (uint64_t(mb_max) << 20) + slab_bytes_;
  cap_slabs_ = uint32_t(std::min<uint64_t>((bytes_cap + slab_bytes_ - 1) / slab_bytes_, kMaxSlabs));
  const uint32_t initial_slabs =
      uint32_t(std::min<uint64_t>(((uint64_t(initial_mb) << 20) + slab_bytes_ - 1) / slab_bytes_, cap_slabs_));

  active_ = true;  // CrearSlab lo necesita puesto
  for (uint32_t i = 0; i < initial_slabs; ++i) {
    if (!CreateSlab(false)) {
      break;
    }
  }
  if (slabs_.empty()) {
    REXLOG_WARN("[nativo] C3: pool de texturas: no se pudo crear ni un bloque de {} MB; se sigue con la "
                "reserva dedicada por textura",
                slab_mb);
    active_ = false;
    vk_device_ = nullptr;
    device_ = VK_NULL_HANDLE;
    return false;
  }

  REXLOG_INFO("[nativo] C3: pool de texturas ENCENDIDO: {} bloques de {} MB precalentados ({} MB), tope {} "
              "bloques ({} MB); tipo de memoria {}, unidad {} KB, alineacion que pide el driver {} KB",
              slabs_.size(), slab_mb, (slabs_.size() * slab_bytes_) >> 20, cap_slabs_,
              (uint64_t(cap_slabs_) * slab_bytes_) >> 20, memory_type_, kPoolUnitBytes >> 10,
              view_alignment >> 10);
  return true;
}

bool TexturePool::CreateSlab(bool in_hot) {
  if (!active_ || slabs_.size() >= cap_slabs_ || slabs_.size() >= kMaxSlabs) {
    return false;
  }
  /*
   * Without VkMemoryDedicatedAllocateInfo on purpose: that way the nvkmd_mem is created with
   * pte_kind = 0 and tile_mode = 0, its layout.valid stays false
   * (nvkmd/switch/nvkmd_switch_dev.c:787-795) and the block accepts aliased images with any
   * pte_kind (horizon/nouveau_horizon_memory.c:1056-1073).
   */
  VkMemoryAllocateInfo reservation{};
  reservation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  reservation.allocationSize = slab_bytes_;
  reservation.memoryTypeIndex = memory_type_;

  VkDeviceMemory mem = VK_NULL_HANDLE;
  if (vk_device_->functions().vkAllocateMemory(device_, &reservation, nullptr, &mem) != VK_SUCCESS) {
    ++failed_slabs_;
    return false;
  }

  Slab slab;
  slab.mem = mem;
  slab.units = units_slab_;
  slab.occupied_items.assign((units_slab_ + 63) / 64, 0ull);
  slab.largo.assign(units_slab_, 0u);
  slabs_.push_back(std::move(slab));
  if (in_hot) {
    ++slabs_in_hot_;
    REXLOG_INFO("[nativo] C3: pool de texturas: bloque {} nuevo en caliente; ya van {} MB reservados",
                slabs_.size() - 1, (slabs_.size() * slab_bytes_) >> 20);
  }
  return true;
}

void TexturePool::Finish() {
  if (device_ != VK_NULL_HANDLE) {
    const auto& dfn = vk_device_->functions();
    for (Slab& slab : slabs_) {
      if (slab.mem != VK_NULL_HANDLE) {
        dfn.vkFreeMemory(device_, slab.mem, nullptr);
      }
    }
  }
  slabs_.clear();
  active_ = false;
  vk_device_ = nullptr;
  device_ = VK_NULL_HANDLE;
}

bool TexturePool::Reserve(const VkMemoryRequirements& requirements, VkDeviceMemory& memory_out,
                            VkDeviceSize& offset_out, uint32_t& block_out) {
  if (!active_) {
    return false;
  }
  // The slab's type must be among the ones this image accepts. On NVK for Tegra it always
  // is, but if some format said otherwise, falling back to the dedicated path is better than
  // binding something invalid.
  if (((requirements.memoryTypeBits >> memory_type_) & 1u) == 0u) {
    return false;
  }
  if (requirements.size == 0) {
    return false;
  }

  const uint64_t units64 = (requirements.size + kPoolUnitBytes - 1) / kPoolUnitBytes;
  if (units64 > units_slab_) {
    ++failed_slots_;
    return false;  // does not fit even in an empty block: use the dedicated path
  }
  const uint32_t units = uint32_t(units64);

  /*
   * The alignment is honored exactly; it is not assumed to be 64 KiB.
   *
   * For TILING_OPTIMAL on Switch the driver asks for at least 64 KiB
   * (nvk_image.c:1192-1197), but a 3D texture can ask for up to 512 KB because the level-0
   * tile size includes z_log2 (nouveau/nil/image.rs:430). Assuming 64 KiB would make the
   * bind fail on a driver assert (nvk_image.c:1692), a rare and late failure. Converted to
   * units, the alignment is the step used to search for candidate starts.
   */
  const uint64_t align_bytes = std::max<uint64_t>(requirements.alignment, kPoolUnitBytes);
  const uint32_t step = uint32_t(std::max<uint64_t>(1, align_bytes / kPoolUnitBytes));

  for (uint32_t s = 0; s < slabs_.size(); ++s) {
    Slab& slab = slabs_[s];
    uint32_t start = 0;
    if (!FindSlot(slab.occupied_items, slab.units, units, step, start)) {
      continue;
    }
    MarkRange(slab.occupied_items, start, units, true);
    slab.largo[start] = units;
    slab.units_in_use += units;

    memory_out = slab.mem;
    offset_out = VkDeviceSize(start) * kPoolUnitBytes;
    block_out = (s << 24) | start;
    ++alive_textures_;
    ++placed_textures_;
    return true;
  }

  ++failed_slots_;
  return false;
}

void TexturePool::Dispose(uint32_t block) {
  if (block == kInvalidPoolBlock || !active_) {
    return;
  }
  const uint32_t s = block >> 24;
  const uint32_t start = block & 0x00FFFFFFu;
  if (s >= slabs_.size()) {
    REXLOG_ERROR("[nativo] C3: pool de texturas: bloque {:08X} con un bloque grande que no existe", block);
    return;
  }
  Slab& slab = slabs_[s];
  if (start >= slab.units || slab.largo[start] == 0u) {
    REXLOG_ERROR("[nativo] C3: pool de texturas: bloque {:08X} que no era el inicio de nada (doble liberacion?)",
                 block);
    return;
  }
  const uint32_t units = slab.largo[start];
  MarkRange(slab.occupied_items, start, units, false);
  slab.largo[start] = 0u;
  slab.units_in_use -= std::min(slab.units_in_use, units);
  if (alive_textures_ > 0) {
    --alive_textures_;
  }
}

void TexturePool::PerFrame(uint64_t frame) {
  if (!active_ || slabs_.size() >= cap_slabs_) {
    return;
  }
  if (frame < last_growth_ + kFramesBetweenSlabs) {
    return;
  }
  uint64_t free_items = 0;
  for (const Slab& slab : slabs_) {
    free_items += uint64_t(slab.units - slab.units_in_use) * kPoolUnitBytes;
  }
  if (free_items >= kSlackBytes) {
    return;
  }
  last_growth_ = frame;
  CreateSlab(true);
}

TexturePoolState TexturePool::State() const {
  TexturePoolState e;
  e.active = active_;
  e.memory_type = memory_type_;
  e.slabs = uint32_t(slabs_.size());
  e.reserved_bytes = uint64_t(slabs_.size()) * slab_bytes_;
  uint32_t larger = 0;
  for (const Slab& slab : slabs_) {
    e.in_use_bytes += uint64_t(slab.units_in_use) * kPoolUnitBytes;
    larger = std::max(larger, SlotGreater(slab.occupied_items, slab.units));
  }
  e.free_bytes = e.reserved_bytes - e.in_use_bytes;
  e.slot_greater_bytes = uint64_t(larger) * kPoolUnitBytes;
  e.alive_textures = alive_textures_;
  e.placed_textures = placed_textures_;
  e.dedicated_textures = dedicated_textures_;
  e.failed_slots = failed_slots_;
  e.slabs_in_hot = slabs_in_hot_;
  e.failed_slabs = failed_slabs_;
  return e;
}

std::string TexturePool::Summary() const {
  if (!active_) {
    return std::string("pool de texturas apagado (") + std::to_string(dedicated_textures_) +
           " texturas con reserva dedicada)";
  }
  const TexturePoolState e = State();
  /*
   * Fragmentation is what needs watching: if plenty is free but the largest free range is
   * small, large textures start falling back to the dedicated path and the savings vanish
   * without anything failing. That is why both figures are printed together.
   */
  const uint64_t frag = e.free_bytes > 0 ? 100u - (e.slot_greater_bytes * 100u / e.free_bytes) : 0u;
  return std::string("pool de texturas: ") + std::to_string(e.slabs) + " bloques (" +
         std::to_string(e.reserved_bytes >> 20) + " MB), " + std::to_string(e.in_use_bytes >> 20) +
         " MB en uso por " + std::to_string(e.alive_textures) + " texturas; libres " +
         std::to_string(e.free_bytes >> 20) + " MB con el mayor hueco en " +
         std::to_string(e.slot_greater_bytes >> 20) + " MB (fragmentacion " + std::to_string(frag) +
         " %); " + std::to_string(e.placed_textures) + " colocadas y " +
         std::to_string(e.dedicated_textures) + " a la ruta dedicada (" + std::to_string(e.failed_slots) +
         " sin hueco); bloques en caliente " + std::to_string(e.slabs_in_hot) + ", negados " +
         std::to_string(e.failed_slabs);
}

}  // namespace nfsmw::nativo
