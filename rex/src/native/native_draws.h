// nfsmw - native renderer, parts C3, C4, C5c and C6: drawing with the NFSSPV library shaders onto the
// C2 render targets.
//
// Started as the version that validated the whole chain on PC with the videos and the title screen: 2D
// textures (base level), guest vertices and indices, constants, pipelines per state and render passes.
// What is not covered is rejected with the cause logged once (details in the .cpp).

#pragma once

#include <rex/ui/vulkan/device.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rex::memory {
class Memory;
}

namespace nfsmw::native_renderer {

struct ShaderEntry;

// Image of a render target or a texture, in a host format.
struct NativeImage {
  VkImage img = VK_NULL_HANDLE;
  // Only if the image has its own dedicated allocation. Images from the texture pool leave this NULL on
  // purpose: their memory is a chunk of a shared slab and cannot be freed on its own.
  VkDeviceMemory mem = VK_NULL_HANDLE;
  // Texture pool slab, or UINT32_MAX if the image does not come from the pool. The literal is used here
  // instead of kBloquePoolInvalido to keep that header out of this one.
  uint32_t block_pool = 0xFFFFFFFFu;
  VkImageView view = VK_NULL_HANDLE;
  uint32_t width_px = 0;
  uint32_t height_px = 0;
  // nfsmw_nativo_sombras_escala: the shadow map is drawn smaller than the guest requests and upscaled when
  // resolved. This holds the size the guest thinks it has; 0 = the same.
  uint32_t guest_width = 0;
  uint32_t guest_height = 0;
  // ZCULL: the scene's depth images are created without TRANSFER_DST so the driver can assign them a ZCULL
  // plane. Without that usage they cannot be cleared with vkCmdClearDepthStencilImage or receive copies:
  // they have to be cleared by opening a pass with loadOp = CLEAR, and they cannot be swapped with their
  // resolved texture.
  bool supports_copy_target = true;
  VkFormat format_id = VK_FORMAT_UNDEFINED;
  bool prepared = false;  // already in GENERAL and initialized
  // Resolved texture with RB_COPY_DEST_INFO.copy_dest_swap: the guest sees it with red and blue swapped
  // relative to the render target it comes from.
  bool rb_swap = false;
  // nfsmw_nativo_resolver_sin_copia: when a whole render target is resolved, its image is swapped with the
  // resolved texture's instead of copied. The target keeps that texture's old content: if the game draws
  // on it again without clearing first, the content has to be brought back.
  bool invalid_content = false;
  uint32_t base_resolved = 0;  // where its content is while contenido_invalido
  // AirborneNX: scale of the stored values relative to what the guest texture holds. A resolve with
  // RB_COPY_DEST_INFO.copy_dest_exp_bias stores 2^bias times the render target (MoH:A resolves its HDR scene
  // with -3); the copy here is image to image, so the shaders multiply the samples by this instead.
  float scale = 1.0f;
};

// GPU time categories (ContextoDestinos::MarcarGpu and the C2 report).
inline constexpr uint32_t kGpuOther = 0;
inline constexpr uint32_t kGpuShadows = 1;   // depth-only targets of 1600 or more
inline constexpr uint32_t kGpuScene = 2;    // 1280
inline constexpr uint32_t kGpuReflection = 3;   // 640
inline constexpr uint32_t kGpu320 = 4;       // 320: cubemap faces and blur
inline constexpr uint32_t kGpuLesser = 5;   // under 320
inline constexpr uint32_t kGpuCopies = 6;    // C2 copies to resolved textures
inline constexpr uint32_t kGpuClears = 7;  // C2 color and depth clears
// 1280 targets without depth: full-screen post-processing and HUD ("scene" is kept for the ones with
// depth, the geometry).
inline constexpr uint32_t kGpuSceneWithoutDepth = 8;
// Not a pass type but the GPU gap between the end of one submission and the start of the next.
inline constexpr uint32_t kGpuSlotBetweenJobs = 9;
inline constexpr uint32_t kGpuCategories = 10;
// Buckets of the histogram of intervals between Swaps (originally <15, 15-18, 18-25, 25-30, 30-36, 36-50,
// >=50 ms). Now 11 buckets: the top ones were all lumped into ">=50 ms", where 83 % of race frames fell,
// so a 55 ms frame could not be told from a 150 ms one, which is exactly the difference between "slow"
// and "stuttering".
inline constexpr uint32_t kSwapBuckets = 11;

// Stages of the C6 report. The last four split the pass change, which on the console is the most
// expensive and most variable stage.
inline constexpr uint32_t kDrawStages = 12;

// What the C2 render target code provides to the draws. All on the ring thread.
class TargetsContext {
 public:
  virtual ~TargetsContext() = default;
  // Command buffer of the frame's work, recording. nullptr on failure.
  virtual VkCommandBuffer JobCommands() = 0;
  // Upload command buffer, recording: submitted right before the work one.
  virtual VkCommandBuffer UploadCommands() = 0;
  // Changes every time the work buffer starts recording again.
  virtual uint64_t CommandsGeneration() const = 0;
  // Render targets already in GENERAL. May record commands: call outside a pass.
  virtual NativeImage* ColorTarget(uint32_t base, uint32_t format_id, uint32_t pitch) = 0;
  virtual NativeImage* DepthTarget(uint32_t base, uint32_t format_id, uint32_t pitch) = 0;
  // Texture resolved by C2 at that physical address, or nullptr.
  virtual const NativeImage* ResolvedTexture(uint32_t address) = 0;
  // AirborneNX: the size of the guest texture that samples the resolved texture at that address. A resolved
  // image is created as wide as RB_COPY_DEST_PITCH (aligned to 32), but the guest samples it with normalized
  // coordinates over its own size (MoH:A's bloom: pitch 352, texture 328). The next resolves create the image
  // with this size instead.
  virtual void TrackTextureSize(uint32_t address, uint32_t width_px, uint32_t height_px) {
    (void)address, (void)width_px, (void)height_px;
  }
  // nfsmw_nativo_profundidad_perezosa. While set, the depth textures requested belong to a sample the
  // shader does not take (the final composite without blur) and do not force a copy.
  virtual void DeadDepthReads(bool dead) { (void)dead; }
  // nfsmw_nativo_diag_borrados. What a pass loads and stores of that render target (its renderArea): the
  // part of what was cleared that is actually used. Called before opening the pass.
  virtual void TrackPassArea(const NativeImage* img, uint32_t width_px, uint32_t height_px) {
    (void)img;
    (void)width_px;
    (void)height_px;
  }
  // nfsmw_nativo_sombra_minimo (see nfsmw_nativo_destinos.cpp). When a pass opens: true if it is the
  // shadow map car pass that C2 is observing or applying, and then each draw is validated with
  // DibujoDeCochesSombra. `solo_profundidad`: the pass has no color targets.
  virtual bool CarShadowPass(const NativeImage* depth_value, bool only_depth) {
    (void)depth_value;
    (void)only_depth;
    return false;
  }
  // A draw of that pass: `exacto` if it leaves in the depth buffer the minimum of what was there and of
  // its fragments.
  virtual void CarShadowDraw(bool exact_val, uint32_t control_z) {
    (void)exact_val;
    (void)control_z;
  }
  // Address of the shadow map texture with cars to watch for when preparing samplers; 0 = none.
  virtual uint32_t CarShadowAddress() const { return 0; }
  // A draw samples that texture. `capaz`: its pixel shader has tfetch2DSombraMin on that register; `ps`,
  // its number. Returns the pair to take the minimum with (its view goes in the register's 3D index word),
  // or nullptr.
  virtual const NativeImage* CarShadowCompanion(bool capable, uint32_t ps) {
    (void)capable;
    (void)ps;
    return nullptr;
  }
  // Submits what was recorded and continues in the other slot, with its upload buffer empty.
  virtual bool SendAndWait() = 0;
  // Waits for the GPU to finish everything pending and starts recording again. It is expensive (a
  // one-frame stutter), so it is only used as a last resort when memory runs out: with the GPU idle,
  // textures can be released regardless of when they were last used, because none is in use.
  virtual bool WaitGpuIdle() = 0;
  // GPU timestamp: whatever is recorded from here counts toward that category.
  virtual void MarkGpu(uint32_t category) = 0;
  // Host occlusion query for the game's open one, inside the open pass. Returns its index, or UINT32_MAX
  // if no game query is open or there is no room left. End it before closing the pass.
  virtual uint32_t StartOcclusionQuery() = 0;
  virtual void FinishOcclusionQuery(uint32_t index) = 0;

  // nfsmw_reflejo_visibilidad. Our own occlusion query around a single draw that samples the reflection
  // (or around the witness, the final composite), inside the open pass and with no game query open.
  // Returns its index, or UINT32_MAX if it cannot be measured (the draw counts as visible). End it right
  // after the draw.
  virtual uint32_t StartVisibilityQuery(bool sentinel) {
    (void)sentinel;
    return UINT32_MAX;
  }
  virtual void FinishVisibilityQuery(uint32_t index) { (void)index; }

  // Pipeline statistics of a whole pass (shaded fragments, vertex invocations and primitives reaching
  // clipping), accumulated per category. UINT32_MAX if not measured.
  virtual uint32_t StartStats(uint32_t category) = 0;
  virtual void FinishStats(uint32_t index) = 0;

  // The same per draw, tagged with the pixel shader number, in an occasional diagnostic frame. Returns
  // UINT32_MAX if not measured or if there is no room left.
  virtual uint32_t StartDrawStats(uint32_t label, uint32_t category) = 0;
  virtual void FinishDrawStats(uint32_t index) = 0;
};

struct DrawRequest {
  const uint32_t* regs = nullptr;       // register mirror of the ring sink
  const ShaderEntry* vs = nullptr;
  const ShaderEntry* ps = nullptr;
  std::span<const uint32_t> microcode_vs;  // patched by D3D (last IM_LOAD)
  uint64_t generacion_vs = 0;                // changes with every VS IM_LOAD
  uint64_t generacion_constantes_vs = 0;     // changes when 0x4000-0x43FF are written
  uint64_t generacion_constantes_ps = 0;     // changes when 0x4400-0x47FF are written
  /*
   * The two most expensive stages of recording a draw on the console are "texturas" (4.1 ms per frame) and
   * the viewport/scissor that the C6 report includes in "pipeline". Both are pure functions of registers
   * that almost never change between consecutive draws, so the ring sink tracks when they really change
   * (it compares the value before writing it, as with the constants) and the count arrives here.
   *
   * A consumer can keep the last value seen next to its result (the 48+16 texture slots and the 32 1/size
   * words of the shared block; the VkViewport, the VkRect2D and ndc[4]) and skip the whole recomputation
   * while it does not change; the framing cache does this with generacion_encuadre. A generation that goes
   * up too often only causes extra work, never a wrong draw.
   */
  uint64_t generacion_fetch = 0;             // changes when 0x4800-0x48BF are written (fetch constants)
  uint64_t framing_generation = 0;          // viewport, tijera, recorte, modo de rasterizado
  // kVeg* flags (native_hooks.h) of the record the draw comes with; 0 = no verdict from the game
  // (nfsmw_d3d_vegetacion_juego).
  uint16_t vegetation_game = 0;
};

struct DrawStats {
  uint64_t drawn = 0;
  uint64_t rejected = 0;
  uint64_t pipelines = 0;
  uint64_t textures = 0;
  uint64_t texture_uploads = 0;
  uint64_t uploaded_megas = 0;  // vertices, indices, constants and textures
  uint64_t textures_megas = 0;  // texture images created (base level, no eviction)
  uint64_t ms_pipelines = 0;  // creando pipelines, acumulado
  // Accumulated for "C6 contadores" (the system prints the differences per report).
  uint64_t passes = 0;             // render passes empezados
  uint64_t full_sends = 0;     // submissions due to a full upload buffer
  uint64_t full_sends_ns = 0;  // inside those submissions, including the wait for the GPU
  uint64_t bytes_vertices = 0;
  // Deduplication of vertex uploads within the frame.
  uint64_t hits_dedupe = 0;
  uint64_t dedupe_bytes = 0;
  uint64_t collisions_dedupe = 0;
  uint64_t bytes_indices = 0;
  uint64_t samplers = 0;          // samplers preparados
  uint64_t samplers_cache = 0;    // of those, resolved by the register cache
  uint64_t passes_ns = 0;          // inside TerminarPase + EmpezarPase on a pass change
  uint64_t ns_vertices = 0;       // copying vertices with byte swap
  uint64_t computed_entries = 0;   // CalcularEntrada due to another VS generation
  uint64_t entries_ns = 0;           // inside CalcularEntrada
  uint64_t reused_entries = 0; // EntradaDe with the same generation and VS
  uint64_t passes_per_generation = 0;  // pass change due to a new command buffer
  uint64_t passes_per_target = 0;     // pass change due to other render targets
  uint64_t resumed_passes = 0;      // the same pass, closed earlier by a copy or a clear
  uint64_t ns_render_pass = 0;        // en vkCmdBeginRenderPass + vkCmdEndRenderPass
  uint64_t passes_texels = 0;          // area opened, summed over every pass opening
  // The same area, split by render target type (indices kGpuSombras..kGpuMenores).
  std::array<uint64_t, kGpuCategories> texels_per_category{};
  // Pass openings per render target type, to know the average area of each.
  std::array<uint64_t, kGpuCategories> passes_per_category{};
  // Draws and triangles per render target type. With each one's GPU time (C2 report) it is possible to
  // tell whether an expensive pass is geometry-bound or pixel-bound: the counts do not depend on the
  // machine, so what is measured on PC holds for the console.
  std::array<uint64_t, kGpuCategories> draws_per_category{};
  // Draws with no color to write, by whether their pixel shader is needed.
  uint64_t useless_ps_draws = 0;
  uint64_t required_ps_draws = 0;
  // Shadow map draws by their constant c1 (g_bShadowMapAlphaEnabled).
  uint64_t shadows_active_alpha = 0;
  uint64_t shadows_alpha_disabled = 0;
  std::array<uint64_t, kGpuCategories> triangles_per_category{};
  // Submissions (upload slot changes) with constants through UBOs and in total, to separate the modes of
  // nfsmw_nativo_constantes_ubo_alternar_s in the report.
  uint64_t ubo_sends = 0;
  uint64_t sends = 0;
  // How many draws really change the 488 bytes of shared constants. It decides whether the comparison or
  // the double memcpy is what needs to get cheaper.
  uint64_t peeked_shared = 0;
  uint64_t changed_shared = 0;
  uint64_t frame_repeated_bytes = 0;  // nfsmw_nativo_diag_vertices_repetidos
  uint64_t previous_equal_bytes = 0;
  uint64_t ns_hash_vertices = 0;
  // Rejections by cause (Rechazar in the .cpp), most frequent first.
  std::vector<std::pair<uint32_t, uint64_t>> causes;
  // Accumulated nanoseconds per stage of the draws that get recorded: state, indices, textures, pass,
  // uploads, pipeline and recording.
  std::array<uint64_t, kDrawStages> stages_ns{};
  // Scene draws that allow or prevent early depth rejection.
  uint64_t scene_with_discard = 0;
  uint64_t scene_without_discard = 0;
  // Recorded draws with the stopwatch (1 in kCronometroCada): the divisor of etapas_ns and ns_vertices.
  uint64_t timed_drawn = 0;
  // Shadow map vegetation dropped right on entry, before paying for indices, textures, upload and pass. It
  // was counted but never printed anywhere, so the gain could not be verified at first. Now it is printed.
  uint64_t vegetation_soon = 0;
};

class VulkanDrawer {
 public:
  // nullptr if the device lacks capabilities (the reason is logged).
  static std::unique_ptr<VulkanDrawer> Make(const rex::ui::vulkan::VulkanDevice* dev,
                                              rex::memory::Memory* mem,
                                              TargetsContext* contexto);
  virtual ~VulkanDrawer() = default;

  virtual bool Draw(const DrawRequest& req_info) = 0;
  // Before copies, clears and any command outside a pass.
  virtual void FinishPass() = 0;
  // ZCULL: clears a depth image by opening a pass with loadOp = CLEAR, instead of with
  // vkCmdClearDepthStencilImage. Needed for images created without TRANSFER_DST, the only ones the driver
  // can give a ZCULL plane. Returns false if it could not.
  virtual bool ClearDepthInPass(VkCommandBuffer commands, const NativeImage& img,
                                       float depth_value, uint32_t stencil) = 0;
  // nfsmw_nativo_borrar_area_util. Clears only the `area` rectangle of a color image by opening a pass
  // with loadOp = CLEAR. Returns false if it could not.
  virtual bool ClearColorInPass(VkCommandBuffer commands, const NativeImage& img, const VkClearColorValue& color,
                                 const VkRect2D& area) {
    (void)commands;
    (void)img;
    (void)color;
    (void)area;
    return false;
  }
  // Right before submitting: closes the pass and publishes the upload buffer.
  virtual void BeforeSend() = 0;
  // Starts work in that work slot. Its upload buffer starts over: the GPU has finished the last work
  // submitted with it.
  virtual void UseSlot(uint32_t ranura) = 0;
  // A C2 image is about to be destroyed: it is removed from the descriptors.
  virtual void ForgetImage(VkImage img) = 0;
  // A C2 view is about to be destroyed (Destruir, in nfsmw_nativo_destinos.cpp, with the GPU idle for that
  // image): the FramebufferDe cache framebuffers that use it are destroyed. Without this, if Vulkan gave
  // the same handle to another view, the cache returned a framebuffer created on the dead view.
  virtual void ForgetView(VkImageView view) { (void)view; }
  // The GPU is out of memory. Stops the GPU, releases half the texture cache and reports whether anything
  // was freed. Called by both texture and render target allocations: whichever runs out of memory first
  // calls this before giving up.
  virtual bool ReleaseTexturesOnMemoryShortage() = 0;
  // Before every C2 copy: a resolved texture may change image or channels.
  virtual void InvalidateTextures() = 0;

  // Invalidates in the texture caches only what points to these two images. Used by the image swap of
  // nfsmw_nativo_resolver_sin_copia: dropping the whole cache twice per frame costs more than the copy it
  // saves (measured on PC).
  virtual void InvalidateImages(VkImage a, VkImage b) = 0;
  virtual DrawStats Stats() const = 0;
  // Draws recorded since start-up; C2 counts those that fall between two copies.
  virtual uint64_t Drawn() const = 0;
  // Waits for the copy thread to finish the pending vertex copies (nfsmw_nativo_subidas_hilo).
  virtual void WaitUploads() = 0;
  // Vertex copies queued and not done yet (only to measure the fences).
  virtual size_t PendingCopies() const { return 0; }
  // With a game occlusion query open, each span of draws inside a pass is counted with a host query
  // (ContextoDestinos::EmpezarConsultaOclusion), which closes with the pass or when the game query closes.
  virtual void OcclusionOpen(bool open) = 0;
};

/*
 * Incremented every time the ring thread handles a guest wait for the GPU (WAIT_REG_MEM). That is the
 * only point at which the game may legally rewrite a vertex range it has already referenced in this
 * frame, so it is where upload deduplication must forget what it recorded. See
 * native_vertex_dedupe.h.
 */
extern std::atomic<uint32_t> g_ring_syncs;

/*
 * The ring thread's reports are written on another thread (nfsmw_nativo_informes_diferidos).
 *
 * The ring's periodic dump (the system report, every 20 s) took 29-32 ms, and not because of formatting:
 * the log is synchronous (log_async = false) and every 2-4 lines the SD FILE is flushed to the card. In
 * one race there were 6 stutters of 60 ms or more: 3 ended 40-70 ms after a dump started, and 1 within
 * 0.12 s of the C6 subetapas line (0.5 would be expected by chance).
 * The line is formatted on the calling thread (it reads the ring's state, which is only consistent on its
 * own thread) and queued; the "NFSMW informes" thread (priority 0x3B) writes it. The queue never blocks
 * the producer: if it fills up, the line is dropped and counted. Defined in nfsmw_nativo_sistema.cpp.
 */
void DeferredReport(std::string line);

// Like REXLOG_INFO, but the report thread does the SD write (InformeDiferido).
#define NFSMW_RING_REPORT(...) ::nfsmw::native_renderer::DeferredReport(fmt::format(__VA_ARGS__))

}  // namespace nfsmw::nativo
