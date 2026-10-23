// UE3 (v421, Xbox 360, big-endian) package decompressor: ue3_decompress <in> <out>
// Compression flags: 2 = LZO1X, 4 = LZX (libmspack lzxd, 17-bit window, per block).
// Exit: 0 ok (output written), 3 not a package, 4 not compressed, other = error.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <mspack.h>
#include <lzx.h>
using namespace std;
typedef vector<uint8_t> Buf;
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0]<<24 | p[1]<<16 | p[2]<<8 | p[3]; }

// ---- LZO1X decompressor (bounds checked) ----
static bool lzo1x(const uint8_t* in, size_t ilen, uint8_t* out, size_t olen) {
  const uint8_t *ip = in, *ie = in + ilen; uint8_t *op = out, *oe = out + olen; const uint8_t* m;
  size_t t;
#define NI(n) if ((size_t)(ie-ip) < (n)) { fprintf(stderr,"NI line %d ip=%zu op=%zu\n",__LINE__,(size_t)(ip-in),(size_t)(op-out)); return false; }
#define NO(n) if ((size_t)(oe-op) < (n)) { fprintf(stderr,"NO line %d ip=%zu op=%zu\n",__LINE__,(size_t)(ip-in),(size_t)(op-out)); return false; }
  NI(1);
  if (*ip > 17) { t = *ip++ - 17; if (t < 4) goto match_next; NI(t); NO(t); do *op++ = *ip++; while (--t); goto first_literal_run; }
  for (;;) {
    NI(1); t = *ip++;
    if (t >= 16) goto match;
    if (t == 0) { for (;;) { NI(1); if (*ip) break; t += 255; ip++; } t += 15 + *ip++; }
    NI(t+3); NO(t+3); for (size_t i = 0; i < t + 3; i++) *op++ = *ip++;
  first_literal_run:
    NI(1); t = *ip++;
    if (t >= 16) goto match;
    NI(1); m = op - (1 + 0x0800) - (t >> 2) - (*ip++ << 2);
    if (m < out) return false; NO(3); *op++ = *m++; *op++ = *m++; *op++ = *m; goto match_done;
  match:
    if (t >= 64) {
      NI(1); m = op - 1 - ((t >> 2) & 7) - (*ip++ << 3); t = (t >> 5) - 1; goto copy_match;
    } else if (t >= 32) {
      t &= 31; if (t == 0) { for (;;) { NI(1); if (*ip) break; t += 255; ip++; } t += 31 + *ip++; }
      NI(2); m = op - 1 - ((ip[0] | ip[1] << 8) >> 2); ip += 2;
    } else if (t >= 16) {
      m = op - ((t & 8) << 11); t &= 7;
      if (t == 0) { for (;;) { NI(1); if (*ip) break; t += 255; ip++; } t += 7 + *ip++; }
      NI(2); m -= (ip[0] | ip[1] << 8) >> 2; ip += 2;
      if (m == op) return op == oe;  // end marker
      m -= 0x4000;
    } else {
      NI(1); m = op - 1 - (t >> 2) - (*ip++ << 2); if (m < out) return false; NO(2); *op++ = *m++; *op++ = *m; goto match_done;
    }
  copy_match:
    if (m < out) return false; NO(t + 2);
    *op++ = *m++; *op++ = *m++; do *op++ = *m++; while (--t);
  match_done:
    t = ip[-2] & 3;
  match_next:
    if (t == 0) continue;
    NI(t); NO(t); do *op++ = *ip++; while (--t);
    NI(1); t = *ip++;
    goto match;
  }
}

// ---- LZX via memory mspack_system ----
struct MemFile { const uint8_t* rd; size_t rsize, pos; uint8_t* wr; size_t wsize; };
static mspack_file* m_open(mspack_system*, const char*, int) { return nullptr; }
static void m_close(mspack_file*) {}
static int m_read(mspack_file* f, void* b, int n) { auto* m = (MemFile*)f; size_t r = m->rsize - m->pos; if ((size_t)n < r) r = n; memcpy(b, m->rd + m->pos, r); m->pos += r; return (int)r; }
static int m_write(mspack_file* f, void* b, int n) { auto* m = (MemFile*)f; if (m->pos + n > m->wsize) return -1; memcpy(m->wr + m->pos, b, n); m->pos += n; return n; }
static int m_seek(mspack_file*, off_t, int) { return 0; }
static off_t m_tell(mspack_file* f) { return ((MemFile*)f)->pos; }
static void m_msg(mspack_file*, const char*, ...) {}
static void* m_alloc(mspack_system*, size_t n) { return malloc(n); }
static void m_free(void* p) { free(p); }
static void m_copy(void* s, void* d, size_t n) { memcpy(d, s, n); }
static mspack_system sys_ = { m_open, m_close, m_read, m_write, m_seek, m_tell, m_msg, m_alloc, m_free, m_copy, nullptr };
static bool lzx(const uint8_t* in, size_t ilen, uint8_t* out, size_t olen) {
  MemFile r{in, ilen, 0, nullptr, 0}, w{nullptr, 0, 0, out, olen};
  auto* s = lzxd_init(&sys_, (mspack_file*)&r, (mspack_file*)&w, 17, 0, 0x8000, (off_t)olen, 0);
  if (!s) return false;
  int st = lzxd_decompress(s, (off_t)olen); lzxd_free(s);
  return st == 0 && w.pos == olen;
}

int main(int argc, char** argv) {
  if (argc != 3) { fprintf(stderr, "usage: ue3_decompress <in> <out>\n"); return 1; }
  FILE* f = fopen(argv[1], "rb"); if (!f) return 2;
  Buf d; { uint8_t t[65536]; size_t n; while ((n = fread(t, 1, sizeof t, f))) d.insert(d.end(), t, t + n); } fclose(f);
  if (d.size() < 64 || be32(d.data()) != 0x9E2A83C1) return 3;
  size_t o = 12; int32_t fn = (int32_t)be32(&d[o]); o += 4 + (fn > 0 ? fn : 0);   // positive: ANSI; (negative UTF-16 not expected)
  o += 4 + 28 + 16; uint32_t gens = be32(&d[o]); o += 4 + 12 * gens;
  if (o + 16 > d.size()) { fprintf(stderr, "header overflow\n"); return 5; }
  uint32_t cflags = be32(&d[o + 8]), cc = be32(&d[o + 12]); o += 16;
  if (!cc) return 4;
  if (cc > 100000 || o + 16ull * cc > d.size()) { fprintf(stderr, "bad chunk count %u\n", cc); return 5; }
  uint64_t total = 0; Buf out;
  for (uint32_t i = 0; i < cc; i++) { uint64_t e = (uint64_t)be32(&d[o+16*i]) + be32(&d[o+16*i+4]); if (e > total) total = e; }
  if (total > (1ull << 30)) { fprintf(stderr, "bad total %llu\n", (unsigned long long)total); return 5; }
  out.assign(total, 0);
  uint32_t first = be32(&d[o]); if (first > d.size()) first = (uint32_t)d.size();
  memcpy(out.data(), d.data(), first < total ? first : total);   // header preceding first chunk
  for (uint32_t i = 0; i < cc; i++) {
    uint32_t uo = be32(&d[o+16*i]), us = be32(&d[o+16*i+4]), co = be32(&d[o+16*i+8]), cs = be32(&d[o+16*i+12]);
    if ((uint64_t)co + cs > d.size() || (uint64_t)uo + us > total || cs < 16) { fprintf(stderr, "chunk %u range bad\n", i); return 5; }
    const uint8_t* c = &d[co];
    if (be32(c) != 0x9E2A83C1) { fprintf(stderr, "chunk %u bad tag\n", i); return 5; }
    uint32_t bs = be32(c + 4); if (!bs) bs = 0x20000;
    uint32_t ctot = be32(c + 8), utot = be32(c + 12);
    if (utot != us) { fprintf(stderr, "chunk %u size mismatch %u/%u\n", i, utot, us); return 5; }
    size_t nb = (utot + bs - 1) / bs, hp = 16 + 8 * nb, dp = hp, up = 0;
    if (hp > cs) { fprintf(stderr, "chunk %u hdr\n", i); return 5; }
    for (size_t b = 0; b < nb; b++) {
      uint32_t bc = be32(c + 16 + 8 * b), bu = be32(c + 20 + 8 * b);
      if (dp + bc > cs || up + bu > us) { fprintf(stderr, "chunk %u block %zu range\n", i, b); return 5; }
      bool ok = cflags == 2 ? lzo1x(c + dp, bc, &out[uo + up], bu) : cflags == 4 ? lzx(c + dp, bc, &out[uo + up], bu) : false;
      if (!ok) { fprintf(stderr, "chunk %u block %zu decompress fail (flags %u)\n", i, b, cflags); return 6; }
      dp += bc; up += bu;
    }
    if (up != us) { fprintf(stderr, "chunk %u short\n", i); return 5; }
  }
  FILE* w = fopen(argv[2], "wb"); if (!w) return 2;
  fwrite(out.data(), 1, out.size(), w); fclose(w);
  return 0;
}
