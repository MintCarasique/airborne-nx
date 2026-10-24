// nfsmw - PNG captures of the game image (see native_capture.h).
//
// The PNG is written uncompressed (deflate "stored" blocks): the SDK does not
// include stb_image_write and this is enough for testing. A 1280x720 capture
// takes about 2.8 MB.

#include "native_capture.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

REXCVAR_DEFINE_INT32(nfsmw_captura_cada_s, 0, "NFSMW",
                     "Guardar una captura PNG de la imagen del juego cada N segundos en capturas/ "
                     "junto al ejecutable (0 = nunca; solo para pruebas)")
    .range(0, 3600);
REXCVAR_DEFINE_INT32(nfsmw_captura_max, 20, "NFSMW", "Maximo de capturas por ejecucion")
    .range(1, 1000);

namespace nfsmw::capture {
namespace {

std::mutex g_mutex;
std::condition_variable g_cv;
bool g_parar = false;
std::thread g_thread;

uint32_t Crc32(const uint8_t* payload, size_t n, uint32_t crc) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      t[i] = c;
    }
    return t;
  }();
  for (size_t i = 0; i < n; ++i) {
    crc = table[(crc ^ payload[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc;
}

void PutBe32(std::vector<uint8_t>& output, uint32_t value) {
  output.push_back(uint8_t(value >> 24));
  output.push_back(uint8_t(value >> 16));
  output.push_back(uint8_t(value >> 8));
  output.push_back(uint8_t(value));
}

void Chunk(std::vector<uint8_t>& output, const char type[4], const std::vector<uint8_t>& payload) {
  PutBe32(output, uint32_t(payload.size()));
  const size_t type_start = output.size();
  output.insert(output.end(), type, type + 4);
  output.insert(output.end(), payload.begin(), payload.end());
  const uint32_t crc =
      Crc32(output.data() + type_start, output.size() - type_start, 0xFFFFFFFFu) ^ 0xFFFFFFFFu;
  PutBe32(output, crc);
}

// RGBX (R8 G8 B8 X8, like rex::ui::RawImage) to 8-bit RGB PNG.
std::vector<uint8_t> CodificarPng(const rex::ui::RawImage& img) {
  const uint32_t width_px = img.width;
  const uint32_t height_px = img.height;
  std::vector<uint8_t> raw;
  raw.reserve(size_t(height_px) * (size_t(width_px) * 3 + 1));
  for (uint32_t y = 0; y < height_px; ++y) {
    raw.push_back(0);  // sin filtro
    const uint8_t* row = img.data.data() + size_t(y) * img.stride;
    for (uint32_t x = 0; x < width_px; ++x) {
      raw.push_back(row[x * 4]);
      raw.push_back(row[x * 4 + 1]);
      raw.push_back(row[x * 4 + 2]);
    }
  }

  // zlib with "stored" blocks and Adler-32 at the end.
  std::vector<uint8_t> zlib = {0x78, 0x01};
  size_t pos = 0;
  do {
    const size_t chunk = std::min<size_t>(65535, raw.size() - pos);
    const bool is_last = pos + chunk == raw.size();
    zlib.push_back(is_last ? 1 : 0);
    zlib.push_back(uint8_t(chunk));
    zlib.push_back(uint8_t(chunk >> 8));
    zlib.push_back(uint8_t(~chunk));
    zlib.push_back(uint8_t(~chunk >> 8));
    zlib.insert(zlib.end(), raw.begin() + pos, raw.begin() + pos + chunk);
    pos += chunk;
  } while (pos < raw.size());
  uint32_t a = 1, b = 0;
  for (uint8_t byte : raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  PutBe32(zlib, (b << 16) | a);

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  PutBe32(ihdr, width_px);
  PutBe32(ihdr, height_px);
  ihdr.push_back(8);  // bits per channel
  ihdr.push_back(2);  // RGB
  ihdr.push_back(0);  // deflate
  ihdr.push_back(0);  // filtro adaptativo
  ihdr.push_back(0);  // sin entrelazado
  Chunk(png, "IHDR", ihdr);
  Chunk(png, "IDAT", zlib);
  Chunk(png, "IEND", {});
  return png;
}

void Loop(std::function<rex::ui::Presenter*()> get_presenter, int cada_s, int max_value) {
  const std::filesystem::path dir_name = rex::filesystem::GetExecutableFolder() / "capturas";
  const auto start = std::chrono::steady_clock::now();
  for (int n = 1; n <= max_value; ++n) {
    {
      std::unique_lock<std::mutex> mutex_lock(g_mutex);
      if (g_cv.wait_for(mutex_lock, std::chrono::seconds(cada_s), [] { return g_parar; })) {
        return;
      }
    }
    rex::ui::Presenter* presenter_ptr = get_presenter ? get_presenter() : nullptr;
    rex::ui::RawImage img;
    if (!presenter_ptr || !presenter_ptr->CaptureGuestOutput(img) || !img.width ||
        !img.height) {
      REXLOG_WARN("[captura] {}: no hay imagen del juego que capturar", n);
      continue;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir_name, ec);
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - start)
                              .count();
    char name[64];
    std::snprintf(name, sizeof(name), "captura_%03d_%04llds.png", n,
                  static_cast<long long>(secs));
    const std::filesystem::path file_path = dir_name / name;
    const std::vector<uint8_t> png = CodificarPng(img);
    std::ofstream file_name(file_path, std::ios::binary);
    file_name.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    if (!file_name) {
      REXLOG_WARN("[captura] No se pudo escribir {}", file_path.string());
      continue;
    }
    REXLOG_INFO("[captura] {} ({}x{})", file_path.string(), img.width, img.height);
  }
}

}  // namespace

void Boot(std::function<rex::ui::Presenter*()> get_presenter) {
  const int cada_s = REXCVAR_GET(nfsmw_captura_cada_s);
  if (cada_s <= 0 || g_thread.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> mutex_lock(g_mutex);
    g_parar = false;
  }
  g_thread = std::thread(Loop, std::move(get_presenter), cada_s,
                       int(REXCVAR_GET(nfsmw_captura_max)));
  REXLOG_INFO("[captura] Una captura cada {} s (maximo {})", cada_s,
              int(REXCVAR_GET(nfsmw_captura_max)));
}

void Parar() {
  {
    std::lock_guard<std::mutex> mutex_lock(g_mutex);
    g_parar = true;
  }
  g_cv.notify_all();
  if (g_thread.joinable()) {
    g_thread.join();
  }
}

}  // namespace nfsmw::captura
