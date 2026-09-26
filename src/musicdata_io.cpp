#include "musicdata_io.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

#include "unk_patterns.inc"

static bool is_supported_version(uint32_t v) {
  static const uint32_t kHandlers[] = {20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 80};
  for (uint32_t h : kHandlers) {
    if (h == v) return true;
  }
  return false;
}

static void assign_utf8_from_wide(std::string& out, const WCHAR* pw, int wlen) {
  if (wlen <= 0) {
    out.clear();
    return;
  }
  // Largest musicdata string is 0x100 bytes. 256 UTF-16 code units expand to at most 768 UTF-8 bytes.
  char u8buf[768];
  int u8len = WideCharToMultiByte(CP_UTF8, 0, pw, wlen, u8buf, static_cast<int>(sizeof(u8buf)), nullptr, nullptr);
  if (u8len > 0) {
    out.assign(u8buf, static_cast<size_t>(u8len));
    return;
  }
  u8len = WideCharToMultiByte(CP_UTF8, 0, pw, wlen, nullptr, 0, nullptr, nullptr);
  if (u8len <= 0) {
    out.clear();
    return;
  }
  out.resize(static_cast<size_t>(u8len));
  WideCharToMultiByte(CP_UTF8, 0, pw, wlen, out.data(), u8len, nullptr, nullptr);
}

static bool size_fits_win32_int(size_t n) { return n <= static_cast<size_t>((std::numeric_limits<int>::max)()); }

static void assign_utf16(std::string& out, const uint8_t* data, size_t nbytes) {
  if (nbytes < 2) {
    out.clear();
    return;
  }
  size_t nwchar = nbytes / 2;
  const WCHAR* pw = reinterpret_cast<const WCHAR*>(data);
  while (nwchar > 0 && pw[nwchar - 1] == 0) --nwchar;
  if (nwchar == 0) {
    out.clear();
    return;
  }
  if (!size_fits_win32_int(nwchar)) {
    out.clear();
    return;
  }
  bool ascii = true;
  for (size_t i = 0; i < nwchar; ++i) {
    if (pw[i] >= 0x80) {
      ascii = false;
      break;
    }
  }
  if (ascii) {
    out.resize(nwchar);
    for (size_t i = 0; i < nwchar; ++i) out[i] = static_cast<char>(pw[i]);
    return;
  }
  assign_utf8_from_wide(out, pw, static_cast<int>(nwchar));
}

static void assign_cp932(std::string& out, const uint8_t* data, size_t nbytes) {
  size_t z = 0;
  while (z < nbytes && data[z] != 0) ++z;
  if (z == 0) {
    out.clear();
    return;
  }
  if (!size_fits_win32_int(z)) {
    out.clear();
    return;
  }
  bool ascii = true;
  for (size_t i = 0; i < z; ++i) {
    if (data[i] >= 0x80) {
      ascii = false;
      break;
    }
  }
  if (ascii) {
    out.assign(reinterpret_cast<const char*>(data), z);
    return;
  }
  wchar_t wbuf[0x101];
  int wlen = MultiByteToWideChar(932, MB_PRECOMPOSED | MB_ERR_INVALID_CHARS, reinterpret_cast<LPCCH>(data), static_cast<int>(z), wbuf, 0x100);
  if (wlen <= 0) wlen = MultiByteToWideChar(932, MB_PRECOMPOSED, reinterpret_cast<LPCCH>(data), static_cast<int>(z), wbuf, 0x100);
  if (wlen <= 0) {
    wlen = MultiByteToWideChar(932, MB_PRECOMPOSED, reinterpret_cast<LPCCH>(data), static_cast<int>(z), nullptr, 0);
    if (wlen <= 0) {
      out.clear();
      return;
    }
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(932, MB_PRECOMPOSED, reinterpret_cast<LPCCH>(data), static_cast<int>(z), w.data(), wlen);
    assign_utf8_from_wide(out, w.data(), wlen);
    return;
  }
  assign_utf8_from_wide(out, wbuf, wlen);
}

struct BinReader {
  const uint8_t* base = nullptr;
  size_t size = 0;
  size_t pos = 0;
  bool ok = true;

  // Overflow-safe: n bytes available from pos.
  bool require(size_t n) const { return ok && n <= size - pos; }

  void fail() { ok = false; }

  void skip(size_t n) {
    if (!require(n)) {
      fail();
      return;
    }
    pos += n;
  }

  uint8_t u8() {
    if (!require(1)) {
      fail();
      return 0;
    }
    return base[pos++];
  }

  uint16_t u16le() {
    if (!require(2)) {
      fail();
      return 0;
    }
    uint16_t v = static_cast<uint16_t>(base[pos] | (base[pos + 1] << 8));
    pos += 2;
    return v;
  }

  uint32_t u32le() {
    if (!require(4)) {
      fail();
      return 0;
    }
    uint32_t v = static_cast<uint32_t>(base[pos] | (base[pos + 1] << 8u) | (base[pos + 2] << 16u) | (base[pos + 3] << 24u));
    pos += 4;
    return v;
  }

  int16_t i16le() {
    uint16_t u = u16le();
    return static_cast<int16_t>(u);
  }

  int32_t i32le() {
    uint32_t u = u32le();
    return static_cast<int32_t>(u);
  }

  void read_bytes(uint8_t* dst, size_t n) {
    if (!require(n)) {
      fail();
      if (n) memset(dst, 0, n);
      return;
    }
    memcpy(dst, base + pos, n);
    pos += n;
  }

  // Largest string field is 0x100 bytes; keep a zero pad for failed ptr() calls.
  const uint8_t* ptr(size_t n) {
    static const uint8_t kZeroPad[0x100] = {};
    if (!require(n)) {
      fail();
      return kZeroPad;
    }
    const uint8_t* r = base + pos;
    pos += n;
    return r;
  }
};

struct BinWriter {
  std::vector<uint8_t> buf;

  void raw(const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    buf.insert(buf.end(), b, b + n);
  }

  void u32le(uint32_t v) {
    uint8_t b[4] = {static_cast<uint8_t>(v & 0xff), static_cast<uint8_t>((v >> 8) & 0xff), static_cast<uint8_t>((v >> 16) & 0xff),
                    static_cast<uint8_t>((v >> 24) & 0xff)};
    raw(b, 4);
  }

  void u16le(uint16_t v) {
    uint8_t b[2] = {static_cast<uint8_t>(v & 0xff), static_cast<uint8_t>((v >> 8) & 0xff)};
    raw(b, 2);
  }

  void i16le(int16_t v) { u16le(static_cast<uint16_t>(v)); }

  void i32le(int32_t v) { u32le(static_cast<uint32_t>(v)); }

  void pad0(size_t n) { buf.insert(buf.end(), n, 0); }

  void write_unk(const uint8_t* p, size_t n) { raw(p, n); }
};

// Byte length of the first max_codepoints UTF-8 code points. ascii is true when every byte in that prefix is < 0x80.
static void utf8_field_prefix(const std::string& utf8, size_t max_codepoints, size_t& byte_len, bool& ascii) {
  size_t i = 0;
  size_t cp = 0;
  ascii = true;
  const size_t n = utf8.size();
  const char* s = utf8.data();
  while (i < n && cp < max_codepoints) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t inc = 1;
    if (c >= 0x80) {
      ascii = false;
      if (c >= 0xf0) inc = 4;
      else if (c >= 0xe0)
        inc = 3;
      else if (c >= 0xc0)
        inc = 2;
    }
    if (i + inc > n) break;
    i += inc;
    ++cp;
  }
  byte_len = i;
}

static void write_string_field(BinWriter& w, const std::string& utf8, size_t field_bytes, bool utf16) {
  // Every on-disk string field is at most 0x100 bytes. Heap fallback covers anything larger.
  uint8_t stack[0x100];
  std::vector<uint8_t> heap;
  uint8_t* field = stack;
  if (field_bytes > sizeof(stack)) {
    heap.assign(field_bytes, 0);
    field = heap.data();
  } else if (field_bytes > 0) {
    memset(stack, 0, field_bytes);
  }

  size_t byte_len = 0;
  bool ascii = true;
  utf8_field_prefix(utf8, field_bytes, byte_len, ascii);
  if (byte_len == 0) {
    w.raw(field, field_bytes);
    return;
  }
  if (!size_fits_win32_int(byte_len)) {
    w.raw(field, field_bytes);
    return;
  }

  if (ascii) {
    if (!utf16) {
      memcpy(field, utf8.data(), byte_len);
    } else {
      size_t nchars = byte_len;
      if (nchars * 2 > field_bytes) nchars = field_bytes / 2;
      const char* s = utf8.data();
      for (size_t i = 0; i < nchars; ++i) field[i * 2] = static_cast<uint8_t>(s[i]);
      // Match the previous byte-truncate: an odd field width keeps one trailing low byte.
      if ((field_bytes & 1u) && byte_len * 2 > field_bytes) field[field_bytes - 1] = static_cast<uint8_t>(s[field_bytes / 2]);
    }
    w.raw(field, field_bytes);
    return;
  }

  wchar_t wbuf[0x200];
  int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(byte_len), wbuf, 0x200);
  if (wlen <= 0) {
    wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(byte_len), nullptr, 0);
    if (wlen <= 0) {
      w.raw(field, field_bytes);
      return;
    }
    std::wstring wide(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(byte_len), wide.data(), wlen);
    if (utf16) {
      size_t nbytes = static_cast<size_t>(wlen) * 2;
      if (nbytes > field_bytes) nbytes = field_bytes;
      memcpy(field, wide.data(), nbytes);
    } else {
      int n = WideCharToMultiByte(932, 0, wide.data(), wlen, nullptr, 0, nullptr, nullptr);
      if (n > 0) {
        std::vector<char> tmp(static_cast<size_t>(n));
        WideCharToMultiByte(932, 0, wide.data(), wlen, tmp.data(), n, nullptr, nullptr);
        size_t copy = static_cast<size_t>(n);
        if (copy > field_bytes) copy = field_bytes;
        memcpy(field, tmp.data(), copy);
      }
    }
    w.raw(field, field_bytes);
    return;
  }

  if (utf16) {
    size_t nbytes = static_cast<size_t>(wlen) * 2;
    if (nbytes > field_bytes) nbytes = field_bytes;
    memcpy(field, wbuf, nbytes);
  } else {
    char cbuf[0x200];
    int n = WideCharToMultiByte(932, 0, wbuf, wlen, cbuf, static_cast<int>(sizeof(cbuf)), nullptr, nullptr);
    if (n <= 0) {
      n = WideCharToMultiByte(932, 0, wbuf, wlen, nullptr, 0, nullptr, nullptr);
      if (n > 0) {
        std::vector<char> tmp(static_cast<size_t>(n));
        WideCharToMultiByte(932, 0, wbuf, wlen, tmp.data(), n, nullptr, nullptr);
        size_t copy = static_cast<size_t>(n);
        if (copy > field_bytes) copy = field_bytes;
        memcpy(field, tmp.data(), copy);
      }
    } else {
      if (static_cast<size_t>(n) > field_bytes) n = static_cast<int>(field_bytes);
      memcpy(field, cbuf, static_cast<size_t>(n));
    }
  }
  w.raw(field, field_bytes);
}

struct SongScratch {
  std::string title, title_ascii, genre, artist, subtitle;
  std::string bga_filename;
  std::string afp_entry;
};

struct SongData {
  uint32_t song_id = 0xffffffffu;

  std::string title;
  std::string title_ascii;
  std::string genre;
  std::string artist;
  std::string subtitle;

  uint32_t texture_title = 0;
  uint32_t texture_artist = 0;
  uint32_t texture_genre = 0;
  uint32_t texture_load = 0;
  uint32_t texture_list = 0;
  uint32_t texture_subtitle = 0;

  uint32_t font_idx = 0;
  int game_version = 0;
  int other_folder = 0;
  int bemani_folder = 0;
  int beginner_rec_folder = 0;
  int iidx_rec_folder = 0;
  int bemani_rec_folder = 0;
  int splittable_diff = 0;
  int unk_unused = 0;

  int SPB_level = 0, SPN_level = 0, SPH_level = 0, SPA_level = 0, SPL_level = 0;
  int DPB_level = 0, DPN_level = 0, DPH_level = 0, DPA_level = 0, DPL_level = 0;

  uint32_t volume = 0;
  int SPB_ident = 0, SPN_ident = 0, SPH_ident = 0, SPA_ident = 0, SPL_ident = 0;
  int DPB_ident = 0, DPN_ident = 0, DPH_ident = 0, DPA_ident = 0, DPL_ident = 0;

  int bga_delay = 0;
  std::string bga_filename;

  uint32_t afp_flag = 0;
  // JSON contains 9 or 10 elements depending on version; store up to 10.
  std::array<std::string, 10> afp_data{};
  int afp_data_count = 0;
};

static void write_one_song_typed(BinWriter& bw, uint32_t version, const SongData& s) {
  if (version == 80) {
    write_string_field(bw, s.title, 0x80, false);
    write_string_field(bw, s.title_ascii, 0x40, false);
    write_string_field(bw, s.genre, 0x80, false);
    write_string_field(bw, s.artist, 0x80, false);
  } else if (version >= 32) {
    write_string_field(bw, s.title, 0x100, true);
    write_string_field(bw, s.title_ascii, 0x40, false);
    write_string_field(bw, s.genre, 0x80, true);
    write_string_field(bw, s.artist, 0x100, true);
    write_string_field(bw, s.subtitle, 0x100, true);
  } else {
    write_string_field(bw, s.title, 0x40, false);
    write_string_field(bw, s.title_ascii, 0x40, false);
    write_string_field(bw, s.genre, 0x40, false);
    write_string_field(bw, s.artist, 0x40, false);
  }

  bw.u32le(s.texture_title);
  bw.u32le(s.texture_artist);
  bw.u32le(s.texture_genre);
  bw.u32le(s.texture_load);
  bw.u32le(s.texture_list);
  if (version >= 32 && version != 80) bw.u32le(s.texture_subtitle);

  bw.u32le(s.font_idx);
  bw.u16le(static_cast<uint16_t>(s.game_version));

  if (version >= 32 && version != 80) {
    bw.u16le(static_cast<uint16_t>(s.other_folder));
    bw.u16le(static_cast<uint16_t>(s.bemani_folder));
    bw.u16le(static_cast<uint16_t>(s.beginner_rec_folder));
    bw.u16le(static_cast<uint16_t>(s.iidx_rec_folder));
    bw.u16le(static_cast<uint16_t>(s.bemani_rec_folder));
    bw.u16le(static_cast<uint16_t>(s.splittable_diff));
    bw.u16le(static_cast<uint16_t>(s.unk_unused));
  } else {
    bw.u16le(static_cast<uint16_t>(s.other_folder));
    bw.u16le(static_cast<uint16_t>(s.bemani_folder));
    bw.u16le(static_cast<uint16_t>(s.splittable_diff));
  }

  if (version >= 27) {
    uint8_t lv[10] = {static_cast<uint8_t>(s.SPB_level), static_cast<uint8_t>(s.SPN_level), static_cast<uint8_t>(s.SPH_level),
                      static_cast<uint8_t>(s.SPA_level), static_cast<uint8_t>(s.SPL_level), static_cast<uint8_t>(s.DPB_level),
                      static_cast<uint8_t>(s.DPN_level), static_cast<uint8_t>(s.DPH_level), static_cast<uint8_t>(s.DPA_level),
                      static_cast<uint8_t>(s.DPL_level)};
    bw.raw(lv, 10);
  } else {
    uint8_t lv[8] = {static_cast<uint8_t>(s.SPN_level), static_cast<uint8_t>(s.SPH_level), static_cast<uint8_t>(s.SPA_level),
                     static_cast<uint8_t>(s.DPN_level), static_cast<uint8_t>(s.DPH_level), static_cast<uint8_t>(s.DPA_level),
                     static_cast<uint8_t>(s.SPB_level), static_cast<uint8_t>(s.DPB_level)};
    bw.raw(lv, 8);
  }

  if (version == 80)
    bw.write_unk(kUnk80, kUnk80_len);
  else if (version >= 32)
    bw.write_unk(kUnk32, kUnk32_len);
  else if (version >= 27)
    bw.write_unk(kUnk27, kUnk27_len);
  else
    bw.write_unk(kUnk26, kUnk26_len);

  bw.u32le(s.song_id);
  bw.u32le(s.volume);

  if (version >= 27) {
    uint8_t id[10] = {static_cast<uint8_t>(s.SPB_ident), static_cast<uint8_t>(s.SPN_ident), static_cast<uint8_t>(s.SPH_ident),
                      static_cast<uint8_t>(s.SPA_ident), static_cast<uint8_t>(s.SPL_ident), static_cast<uint8_t>(s.DPB_ident),
                      static_cast<uint8_t>(s.DPN_ident), static_cast<uint8_t>(s.DPH_ident), static_cast<uint8_t>(s.DPA_ident),
                      static_cast<uint8_t>(s.DPL_ident)};
    bw.raw(id, 10);
  } else {
    uint8_t id[8] = {static_cast<uint8_t>(s.SPN_ident), static_cast<uint8_t>(s.SPH_ident), static_cast<uint8_t>(s.SPA_ident),
                     static_cast<uint8_t>(s.DPN_ident), static_cast<uint8_t>(s.DPH_ident), static_cast<uint8_t>(s.DPA_ident),
                     static_cast<uint8_t>(s.SPB_ident), static_cast<uint8_t>(s.DPB_ident)};
    bw.raw(id, 8);
  }

  bw.i16le(static_cast<int16_t>(s.bga_delay));

  if (version <= 26 || version == 80) bw.pad0(2);

  write_string_field(bw, s.bga_filename, 0x20, false);

  if (version == 80) bw.pad0(2);

  bw.u32le(s.afp_flag);

  const int afp_count = (version >= 22) ? 10 : 9;
  static const std::string kEmpty;
  for (int idx = 0; idx < afp_count; ++idx) {
    const std::string& x = (idx < s.afp_data_count) ? s.afp_data[static_cast<size_t>(idx)] : kEmpty;
    write_string_field(bw, x, 0x20, false);
  }

  if (version >= 26) bw.pad0(4);
}


// Fixed song-record sizes from write_one_song_typed. Used only to reserve the output buffer.
static size_t song_record_bytes(uint32_t version) {
  if (version == 80) return 1584;
  if (version >= 32) return 2040;
  if (version >= 27) return 1324;
  if (version >= 26) return 836;
  if (version >= 22) return 832;
  return 800;
}

static void writer_impl_typed(uint32_t version, BinWriter& bw, const std::vector<SongData>& songs) {
  const int song_count = static_cast<int>(songs.size());
  uint32_t cur_style_entries = version * 1000u;
  uint32_t max_entries = cur_style_entries + 1000u;
  bool wide_index = (version >= 32 && version != 80);
  const size_t entry_sz = wide_index ? 4u : 2u;

  // Last duplicate song_id wins, matching the previous unordered_map assignment.
  std::vector<std::pair<uint32_t, int>> order;
  order.reserve(static_cast<size_t>(song_count));
  for (int i = 0; i < song_count; ++i) order.emplace_back(songs[static_cast<size_t>(i)].song_id, i);
  std::stable_sort(order.begin(), order.end(), [](const std::pair<uint32_t, int>& a, const std::pair<uint32_t, int>& b) { return a.first < b.first; });

  std::vector<std::pair<uint32_t, int>> uniq;
  uniq.reserve(order.size());
  for (size_t i = 0; i < order.size();) {
    size_t j = i + 1;
    while (j < order.size() && order[j].first == order[i].first) ++j;
    uniq.push_back(order[j - 1]);
    i = j;
  }

  bw.buf.reserve(16 + static_cast<size_t>(max_entries) * entry_sz + static_cast<size_t>(song_count) * song_record_bytes(version));

  bw.raw("IIDX", 4);
  if (version >= 32) {
    bw.u32le(version);
    bw.u16le(static_cast<uint16_t>(song_count));
    bw.u16le(0);
    bw.u32le(max_entries);
  } else {
    bw.u32le(version);
    bw.u16le(static_cast<uint16_t>(song_count));
    bw.u16le(static_cast<uint16_t>(max_entries));
    bw.u32le(0);
  }

  const size_t index_off = bw.buf.size();
  const size_t index_bytes = static_cast<size_t>(max_entries) * entry_sz;
  bw.buf.resize(index_off + index_bytes, 0);
  uint8_t* index = bw.buf.data() + index_off;
  const size_t neg_bytes = static_cast<size_t>(cur_style_entries) * entry_sz;
  if (neg_bytes) memset(index, 0xFF, neg_bytes);

  for (size_t rank = 0; rank < uniq.size(); ++rank) {
    const uint32_t id = uniq[rank].first;
    if (id >= max_entries) continue;
    uint8_t* slot = index + static_cast<size_t>(id) * entry_sz;
    if (wide_index) {
      const uint32_t u = static_cast<uint32_t>(static_cast<int>(rank));
      slot[0] = static_cast<uint8_t>(u);
      slot[1] = static_cast<uint8_t>(u >> 8);
      slot[2] = static_cast<uint8_t>(u >> 16);
      slot[3] = static_cast<uint8_t>(u >> 24);
    } else {
      const uint16_t u = static_cast<uint16_t>(static_cast<int16_t>(rank));
      slot[0] = static_cast<uint8_t>(u);
      slot[1] = static_cast<uint8_t>(u >> 8);
    }
  }

  for (const auto& e : uniq) {
    const int idx = e.second;
    if (idx < 0 || idx >= song_count) continue;
    write_one_song_typed(bw, version, songs[static_cast<size_t>(idx)]);
  }
}

// Pretty-print JSON to match Python 3 json.dump(..., indent=4, ensure_ascii=False).
constexpr int kJsonIndentCols = 4;

static void append_indent_units(std::string& out, int indent_units) {
  if (indent_units <= 0) return;
  out.append(static_cast<size_t>(indent_units) * static_cast<size_t>(kJsonIndentCols), ' ');
}

static void append_u64_digits(std::string& out, uint64_t u) {
  char buf[20];
  char* p = buf + sizeof(buf);
  do {
    *--p = static_cast<char>('0' + static_cast<unsigned>(u % 10));
    u /= 10;
  } while (u);
  out.append(p, static_cast<size_t>((buf + sizeof(buf)) - p));
}

static void append_json_int(std::string& out, long long v) {
  if (v < 0) {
    out.push_back('-');
    // Works for LLONG_MIN, where negating the value itself is undefined.
    append_u64_digits(out, static_cast<uint64_t>(-(v + 1)) + 1ull);
    return;
  }
  append_u64_digits(out, static_cast<uint64_t>(v));
}

static void append_json_u32(std::string& out, uint32_t v) { append_u64_digits(out, v); }

static void append_json_i16(std::string& out, int16_t v) { append_json_int(out, static_cast<long long>(v)); }
static void append_json_i32(std::string& out, int32_t v) { append_json_int(out, static_cast<long long>(v)); }
static void append_json_u16(std::string& out, uint16_t v) { append_json_int(out, static_cast<long long>(v)); }

static void append_escaped_json_string(std::string& out, const char* input) {
  out.push_back('"');
  if (!input || !*input) {
    out.push_back('"');
    return;
  }
  const unsigned char* p = reinterpret_cast<const unsigned char*>(input);
  const unsigned char* chunk = p;
  for (;;) {
    const unsigned char c = *p;
    if (c == 0) {
      out.append(reinterpret_cast<const char*>(chunk), static_cast<size_t>(p - chunk));
      break;
    }
    if (c >= 0x20 && c != '"' && c != '\\') {
      ++p;
      continue;
    }
    if (p != chunk) out.append(reinterpret_cast<const char*>(chunk), static_cast<size_t>(p - chunk));
    switch (c) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\b':
        out.append("\\b");
        break;
      case '\f':
        out.append("\\f");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default: {
        char buf[7];
        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
        out.append(buf);
        break;
      }
    }
    ++p;
    chunk = p;
  }
  out.push_back('"');
}

static void append_afp_data_from_bin_python(std::string& out, BinReader& br, uint32_t version, SongScratch& scratch, int key_line_indent_units) {
  const int afp_n = (version >= 22) ? 10 : 9;
  out += "[\n";
  for (int i = 0; i < afp_n; ++i) {
    assign_cp932(scratch.afp_entry, br.ptr(0x20), 0x20);
    append_indent_units(out, key_line_indent_units + 1);
    append_escaped_json_string(out, scratch.afp_entry.c_str());
    if (i + 1 < afp_n)
      out += ",\n";
    else
      out += '\n';
  }
  append_indent_units(out, key_line_indent_units);
  out += ']';
}

static void append_one_song_from_bin_python(std::string& out, BinReader& br, uint32_t version, SongScratch& scratch, int array_key_line_units) {
  // Formatting matches the plugin's Python-style pretty JSON (indent=4, ensure_ascii=False).
  scratch.subtitle.clear();

  if (version == 80) {
    assign_cp932(scratch.title, br.ptr(0x80), 0x80);
    assign_cp932(scratch.title_ascii, br.ptr(0x40), 0x40);
    assign_cp932(scratch.genre, br.ptr(0x80), 0x80);
    assign_cp932(scratch.artist, br.ptr(0x80), 0x80);
  } else if (version >= 32) {
    assign_utf16(scratch.title, br.ptr(0x100), 0x100);
    assign_cp932(scratch.title_ascii, br.ptr(0x40), 0x40);
    assign_utf16(scratch.genre, br.ptr(0x80), 0x80);
    assign_utf16(scratch.artist, br.ptr(0x100), 0x100);
    assign_utf16(scratch.subtitle, br.ptr(0x100), 0x100);
  } else {
    assign_cp932(scratch.title, br.ptr(0x40), 0x40);
    assign_cp932(scratch.title_ascii, br.ptr(0x40), 0x40);
    assign_cp932(scratch.genre, br.ptr(0x40), 0x40);
    assign_cp932(scratch.artist, br.ptr(0x40), 0x40);
  }

  const uint32_t texture_title = br.u32le();
  const uint32_t texture_artist = br.u32le();
  const uint32_t texture_genre = br.u32le();
  const uint32_t texture_load = br.u32le();
  const uint32_t texture_list = br.u32le();
  uint32_t texture_subtitle = 0;
  if (version >= 32 && version != 80) texture_subtitle = br.u32le();

  const uint32_t font_idx = br.u32le();
  const uint16_t game_version = br.u16le();

  uint16_t other_folder = 0, bemani_folder = 0, beginner_rec_folder = 0, iidx_rec_folder = 0, bemani_rec_folder = 0, splittable_diff = 0, unk_unused = 0;
  if (version >= 32 && version != 80) {
    other_folder = br.u16le();
    bemani_folder = br.u16le();
    beginner_rec_folder = br.u16le();
    iidx_rec_folder = br.u16le();
    bemani_rec_folder = br.u16le();
    splittable_diff = br.u16le();
    unk_unused = br.u16le();
  } else {
    other_folder = br.u16le();
    bemani_folder = br.u16le();
    splittable_diff = br.u16le();
  }

  int SPB_level = 0, SPN_level = 0, SPH_level = 0, SPA_level = 0, SPL_level = 0;
  int DPB_level = 0, DPN_level = 0, DPH_level = 0, DPA_level = 0, DPL_level = 0;
  if (version >= 27) {
    SPB_level = static_cast<int>(br.u8());
    SPN_level = static_cast<int>(br.u8());
    SPH_level = static_cast<int>(br.u8());
    SPA_level = static_cast<int>(br.u8());
    SPL_level = static_cast<int>(br.u8());
    DPB_level = static_cast<int>(br.u8());
    DPN_level = static_cast<int>(br.u8());
    DPH_level = static_cast<int>(br.u8());
    DPA_level = static_cast<int>(br.u8());
    DPL_level = static_cast<int>(br.u8());
  } else {
    SPN_level = static_cast<int>(br.u8());
    SPH_level = static_cast<int>(br.u8());
    SPA_level = static_cast<int>(br.u8());
    DPN_level = static_cast<int>(br.u8());
    DPH_level = static_cast<int>(br.u8());
    DPA_level = static_cast<int>(br.u8());
    SPB_level = static_cast<int>(br.u8());
    DPB_level = static_cast<int>(br.u8());
    SPL_level = 0;
    DPL_level = 0;
  }

  if (version == 80)
    br.skip(0x2C6);
  else if (version >= 27)
    br.skip(0x286);
  else
    br.skip(0xA0);

  const uint32_t song_id = br.u32le();
  const uint32_t volume = br.u32le();

  int SPB_ident = 0, SPN_ident = 0, SPH_ident = 0, SPA_ident = 0, SPL_ident = 0;
  int DPB_ident = 0, DPN_ident = 0, DPH_ident = 0, DPA_ident = 0, DPL_ident = 0;
  if (version >= 27) {
    SPB_ident = static_cast<int>(br.u8());
    SPN_ident = static_cast<int>(br.u8());
    SPH_ident = static_cast<int>(br.u8());
    SPA_ident = static_cast<int>(br.u8());
    SPL_ident = static_cast<int>(br.u8());
    DPB_ident = static_cast<int>(br.u8());
    DPN_ident = static_cast<int>(br.u8());
    DPH_ident = static_cast<int>(br.u8());
    DPA_ident = static_cast<int>(br.u8());
    DPL_ident = static_cast<int>(br.u8());
  } else {
    SPN_ident = static_cast<int>(br.u8());
    SPH_ident = static_cast<int>(br.u8());
    SPA_ident = static_cast<int>(br.u8());
    DPN_ident = static_cast<int>(br.u8());
    DPH_ident = static_cast<int>(br.u8());
    DPA_ident = static_cast<int>(br.u8());
    SPB_ident = static_cast<int>(br.u8());
    DPB_ident = static_cast<int>(br.u8());
    SPL_ident = 48;
    DPL_ident = 48;
  }

  const int16_t bga_delay = br.i16le();
  if (version <= 26 || version == 80) br.skip(2);

  assign_cp932(scratch.bga_filename, br.ptr(0x20), 0x20);
  if (version == 80) br.skip(2);

  const uint32_t afp_flag = br.u32le();

  out += "{\n";
  auto key = [&](const char* k) {
    append_indent_units(out, array_key_line_units + 2);
    out.push_back('"');
    out += k;
    out += "\": ";
  };
  auto comma_nl = [&](bool more) {
    if (more)
      out += ",\n";
    else
      out += '\n';
  };

  key("song_id");
  append_json_u32(out, song_id);
  comma_nl(true);
  key("title");
  append_escaped_json_string(out, scratch.title.c_str());
  comma_nl(true);
  key("title_ascii");
  append_escaped_json_string(out, scratch.title_ascii.c_str());
  comma_nl(true);
  key("genre");
  append_escaped_json_string(out, scratch.genre.c_str());
  comma_nl(true);
  key("artist");
  append_escaped_json_string(out, scratch.artist.c_str());
  comma_nl(true);
  key("texture_title");
  append_json_u32(out, texture_title);
  comma_nl(true);
  key("texture_artist");
  append_json_u32(out, texture_artist);
  comma_nl(true);
  key("texture_genre");
  append_json_u32(out, texture_genre);
  comma_nl(true);
  key("texture_load");
  append_json_u32(out, texture_load);
  comma_nl(true);
  key("texture_list");
  append_json_u32(out, texture_list);
  comma_nl(true);
  key("font_idx");
  append_json_u32(out, font_idx);
  comma_nl(true);
  key("game_version");
  append_json_u16(out, game_version);
  comma_nl(true);
  key("other_folder");
  append_json_u16(out, other_folder);
  comma_nl(true);
  key("bemani_folder");
  append_json_u16(out, bemani_folder);
  comma_nl(true);
  key("splittable_diff");
  append_json_u16(out, splittable_diff);
  comma_nl(true);
  key("SPB_level");
  append_json_i32(out, SPB_level);
  comma_nl(true);
  key("SPN_level");
  append_json_i32(out, SPN_level);
  comma_nl(true);
  key("SPH_level");
  append_json_i32(out, SPH_level);
  comma_nl(true);
  key("SPA_level");
  append_json_i32(out, SPA_level);
  comma_nl(true);
  key("SPL_level");
  append_json_i32(out, SPL_level);
  comma_nl(true);
  key("DPB_level");
  append_json_i32(out, DPB_level);
  comma_nl(true);
  key("DPN_level");
  append_json_i32(out, DPN_level);
  comma_nl(true);
  key("DPH_level");
  append_json_i32(out, DPH_level);
  comma_nl(true);
  key("DPA_level");
  append_json_i32(out, DPA_level);
  comma_nl(true);
  key("DPL_level");
  append_json_i32(out, DPL_level);
  comma_nl(true);
  key("volume");
  append_json_u32(out, volume);
  comma_nl(true);
  key("SPB_ident");
  append_json_i32(out, SPB_ident);
  comma_nl(true);
  key("SPN_ident");
  append_json_i32(out, SPN_ident);
  comma_nl(true);
  key("SPH_ident");
  append_json_i32(out, SPH_ident);
  comma_nl(true);
  key("SPA_ident");
  append_json_i32(out, SPA_ident);
  comma_nl(true);
  key("SPL_ident");
  append_json_i32(out, SPL_ident);
  comma_nl(true);
  key("DPB_ident");
  append_json_i32(out, DPB_ident);
  comma_nl(true);
  key("DPN_ident");
  append_json_i32(out, DPN_ident);
  comma_nl(true);
  key("DPH_ident");
  append_json_i32(out, DPH_ident);
  comma_nl(true);
  key("DPA_ident");
  append_json_i32(out, DPA_ident);
  comma_nl(true);
  key("DPL_ident");
  append_json_i32(out, DPL_ident);
  comma_nl(true);
  key("bga_filename");
  append_escaped_json_string(out, scratch.bga_filename.c_str());
  comma_nl(true);
  key("bga_delay");
  append_json_i16(out, bga_delay);
  comma_nl(true);
  key("afp_flag");
  append_json_u32(out, afp_flag);
  comma_nl(true);
  key("afp_data");
  append_afp_data_from_bin_python(out, br, version, scratch, array_key_line_units + 2);

  if (version >= 32 && version != 80) {
    comma_nl(true);
    key("subtitle");
    append_escaped_json_string(out, scratch.subtitle.c_str());
    comma_nl(true);
    key("texture_subtitle");
    append_json_u32(out, texture_subtitle);
    comma_nl(true);
    key("beginner_rec_folder");
    append_json_u16(out, beginner_rec_folder);
    comma_nl(true);
    key("iidx_rec_folder");
    append_json_u16(out, iidx_rec_folder);
    comma_nl(true);
    key("bemani_rec_folder");
    append_json_u16(out, bemani_rec_folder);
    comma_nl(true);
    key("unk_unused");
    append_json_u16(out, unk_unused);
    comma_nl(false);
  } else {
    comma_nl(false);
  }

  append_indent_units(out, array_key_line_units + 1);
  out += '}';

  if (version >= 26) br.skip(4);
}

// -------- Streaming JSON parser for save path (schema-tailored) --------

namespace {

static inline bool json_is_ws(unsigned char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

struct JsonReader {
  const char* p = nullptr;
  const char* end = nullptr;

  explicit JsonReader(const std::string& s) : p(s.data()), end(s.data() + s.size()) {}
  JsonReader(const char* data, size_t n) : p(data ? data : ""), end((data ? data : "") + (data ? n : 0)) {}

  bool eof() const { return p >= end; }

  void skip_ws() {
    while (!eof() && json_is_ws(static_cast<unsigned char>(*p))) ++p;
  }

  bool consume(char c) {
    skip_ws();
    if (!eof() && *p == c) {
      ++p;
      return true;
    }
    return false;
  }

  bool peek(char c) {
    skip_ws();
    return (!eof() && *p == c);
  }

  bool expect(char c, std::wstring& err) {
    if (consume(c)) return true;
    err = L"Invalid JSON (expected MusicDataPlugin schema): missing expected token.";
    return false;
  }

  static void append_utf8_codepoint(std::string& out, uint32_t cp) {
    if (cp <= 0x7Fu) {
      out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
      out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
      out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
      out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
  }

  bool read_hex4(uint32_t& out, std::wstring& err) {
    out = 0;
    for (int i = 0; i < 4; ++i) {
      if (eof()) {
        err = L"Invalid JSON: truncated \\u escape.";
        return false;
      }
      char c = *p++;
      out <<= 4;
      if (c >= '0' && c <= '9')
        out |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        out |= static_cast<uint32_t>(10 + (c - 'a'));
      else if (c >= 'A' && c <= 'F')
        out |= static_cast<uint32_t>(10 + (c - 'A'));
      else {
        err = L"Invalid JSON: bad hex in \\u escape.";
        return false;
      }
    }
    return true;
  }

  bool read_string(std::string& out, std::wstring& err) {
    skip_ws();
    if (eof() || *p != '"') {
      err = L"Invalid JSON (expected MusicDataPlugin schema): expected string.";
      return false;
    }
    ++p;
    const char* start = p;
    while (p < end && *p != '"' && *p != '\\') ++p;
    if (p < end && *p == '"') {
      out.assign(start, static_cast<size_t>(p - start));
      ++p;
      return true;
    }
    out.assign(start, static_cast<size_t>(p - start));
    while (!eof()) {
      unsigned char c = static_cast<unsigned char>(*p++);
      if (c == '"') return true;
      if (c == '\\') {
        if (eof()) {
          err = L"Invalid JSON: truncated escape.";
          return false;
        }
        char e = *p++;
        switch (e) {
          case '"':
          case '\\':
          case '/':
            out.push_back(e);
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'u': {
            uint32_t cp = 0;
            if (!read_hex4(cp, err)) return false;
            // Surrogates not expected in plugin output; treat as code point.
            append_utf8_codepoint(out, cp);
            break;
          }
          default:
            err = L"Invalid JSON: unknown escape.";
            return false;
        }
      } else {
        out.push_back(static_cast<char>(c));
      }
    }
    err = L"Invalid JSON: unterminated string.";
    return false;
  }

  // Schema keys are short and unescaped. too_long marks a key that cannot be one of ours.
  bool read_key(char* buf, size_t cap, size_t& len, bool& too_long, std::wstring& err) {
    skip_ws();
    if (eof() || *p != '"') {
      err = L"Invalid JSON (expected MusicDataPlugin schema): expected string.";
      return false;
    }
    const char* saved = p;
    ++p;
    const char* start = p;
    while (p < end && *p != '"' && *p != '\\') ++p;
    if (p < end && *p == '"') {
      size_t n = static_cast<size_t>(p - start);
      ++p;
      too_long = n > cap;
      if (!too_long && n) memcpy(buf, start, n);
      len = too_long ? 0 : n;
      return true;
    }
    p = saved;
    std::string tmp;
    if (!read_string(tmp, err)) return false;
    too_long = tmp.size() > cap;
    if (!too_long && !tmp.empty()) memcpy(buf, tmp.data(), tmp.size());
    len = too_long ? 0 : tmp.size();
    return true;
  }

  bool read_null(std::wstring& err) {
    skip_ws();
    if (end - p >= 4 && p[0] == 'n' && p[1] == 'u' && p[2] == 'l' && p[3] == 'l') {
      p += 4;
      return true;
    }
    err = L"Invalid JSON: expected null.";
    return false;
  }

  bool read_i64(int64_t& out, std::wstring& err) {
    skip_ws();
    if (eof()) {
      err = L"Invalid JSON: expected number.";
      return false;
    }
    const char* start = p;
    if (*p == '-' || *p == '+') ++p;
    const char* after_sign = p;
    bool saw_digit = false;
    bool integer_only = true;
    while (!eof()) {
      char c = *p;
      if (c >= '0' && c <= '9') {
        saw_digit = true;
        ++p;
        continue;
      }
      if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
        integer_only = false;
        ++p;
        continue;
      }
      break;
    }
    if (!saw_digit || p == start) {
      err = L"Invalid JSON: expected number.";
      return false;
    }
    if (!integer_only) {
      char* endp = nullptr;
      const double d = std::strtod(start, &endp);
      if (!endp || endp != p) {
        err = L"Invalid JSON: bad number.";
        return false;
      }
      if (!std::isfinite(d)) {
        out = 0;
        return true;
      }
      if (d >= 9223372036854775808.0) {
        out = std::numeric_limits<int64_t>::max();
        return true;
      }
      if (d < static_cast<double>(std::numeric_limits<int64_t>::min())) {
        out = std::numeric_limits<int64_t>::min();
        return true;
      }
      out = static_cast<int64_t>(d);
      return true;
    }
    uint64_t mag = 0;
    const auto res = std::from_chars(after_sign, p, mag);
    if (res.ec == std::errc::result_out_of_range) {
      out = (start[0] == '-') ? std::numeric_limits<int64_t>::min() : std::numeric_limits<int64_t>::max();
      return true;
    }
    if (res.ec != std::errc{} || res.ptr != p) {
      err = L"Invalid JSON: bad number.";
      return false;
    }
    if (start[0] == '-') {
      const uint64_t min_mag = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1ull;
      out = (mag >= min_mag) ? std::numeric_limits<int64_t>::min() : -static_cast<int64_t>(mag);
    } else if (mag > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      out = std::numeric_limits<int64_t>::max();
    } else {
      out = static_cast<int64_t>(mag);
    }
    return true;
  }

  bool skip_value(std::wstring& err, int depth = 0) {
    static constexpr int kMaxJsonDepth = 64;
    if (depth > kMaxJsonDepth) {
      err = L"Invalid JSON: nesting too deep.";
      return false;
    }
    skip_ws();
    if (eof()) {
      err = L"Invalid JSON: unexpected end.";
      return false;
    }
    char c = *p;
    if (c == '"') {
      std::string tmp;
      return read_string(tmp, err);
    }
    if (c == '{') {
      ++p;
      skip_ws();
      if (consume('}')) return true;
      while (true) {
        char kbuf[32];
        size_t klen = 0;
        bool too_long = false;
        if (!read_key(kbuf, sizeof(kbuf), klen, too_long, err)) return false;
        if (!expect(':', err)) return false;
        if (!skip_value(err, depth + 1)) return false;
        if (consume('}')) return true;
        if (!expect(',', err)) return false;
      }
    }
    if (c == '[') {
      ++p;
      skip_ws();
      if (consume(']')) return true;
      while (true) {
        if (!skip_value(err, depth + 1)) return false;
        if (consume(']')) return true;
        if (!expect(',', err)) return false;
      }
    }
    if (c == 'n') return read_null(err);
    int64_t ignored = 0;
    return read_i64(ignored, err);
  }
};

enum class SongField : uint8_t {
  Unknown = 0,
  SongId,
  Title,
  TitleAscii,
  Genre,
  Artist,
  Subtitle,
  TextureTitle,
  TextureArtist,
  TextureGenre,
  TextureLoad,
  TextureList,
  TextureSubtitle,
  FontIdx,
  GameVersion,
  OtherFolder,
  BemaniFolder,
  BeginnerRec,
  IidxRec,
  BemaniRec,
  Splittable,
  UnkUnused,
  SPB_level,
  SPN_level,
  SPH_level,
  SPA_level,
  SPL_level,
  DPB_level,
  DPN_level,
  DPH_level,
  DPA_level,
  DPL_level,
  Volume,
  SPB_ident,
  SPN_ident,
  SPH_ident,
  SPA_ident,
  SPL_ident,
  DPB_ident,
  DPN_ident,
  DPH_ident,
  DPA_ident,
  DPL_ident,
  BgaFilename,
  BgaDelay,
  AfpFlag,
  AfpData,
  DataVer,
  Data,
};

static int chart_slot(const char* s) {
  if (s[1] != 'P' || (s[0] != 'S' && s[0] != 'D')) return -1;
  const int base = (s[0] == 'S') ? 0 : 5;
  switch (s[2]) {
    case 'B':
      return base + 0;
    case 'N':
      return base + 1;
    case 'H':
      return base + 2;
    case 'A':
      return base + 3;
    case 'L':
      return base + 4;
    default:
      return -1;
  }
}

template <size_t N>
static bool key_is(const char* s, size_t n, const char (&lit)[N]) {
  return n == N - 1 && memcmp(s, lit, N - 1) == 0;
}

static SongField classify_field(const char* s, size_t n) {
  switch (n) {
    case sizeof("data") - 1:
      if (key_is(s, n, "data")) return SongField::Data;
      break;
    case sizeof("title") - 1:
      static_assert(sizeof("genre") == sizeof("title"));
      if (key_is(s, n, "title")) return SongField::Title;
      if (key_is(s, n, "genre")) return SongField::Genre;
      break;
    case sizeof("artist") - 1:
      static_assert(sizeof("volume") == sizeof("artist"));
      if (key_is(s, n, "artist")) return SongField::Artist;
      if (key_is(s, n, "volume")) return SongField::Volume;
      break;
    case sizeof("song_id") - 1:
      if (key_is(s, n, "song_id")) return SongField::SongId;
      break;
    case sizeof("font_idx") - 1:
      static_assert(sizeof("subtitle") == sizeof("font_idx"));
      static_assert(sizeof("afp_flag") == sizeof("font_idx"));
      static_assert(sizeof("afp_data") == sizeof("font_idx"));
      static_assert(sizeof("data_ver") == sizeof("font_idx"));
      if (key_is(s, n, "subtitle")) return SongField::Subtitle;
      if (key_is(s, n, "font_idx")) return SongField::FontIdx;
      if (key_is(s, n, "afp_flag")) return SongField::AfpFlag;
      if (key_is(s, n, "afp_data")) return SongField::AfpData;
      if (key_is(s, n, "data_ver")) return SongField::DataVer;
      break;
    case sizeof("bga_delay") - 1: {
      static_assert(sizeof("SPB_level") == sizeof("bga_delay"));
      static_assert(sizeof("SPB_ident") == sizeof("bga_delay"));
      if (key_is(s, n, "bga_delay")) return SongField::BgaDelay;
      const int slot = chart_slot(s);
      if (slot >= 0 && s[3] == '_') {
        if (s[4] == 'l' && memcmp(s + 4, "level", 5) == 0)
          return static_cast<SongField>(static_cast<int>(SongField::SPB_level) + slot);
        if (s[4] == 'i' && memcmp(s + 4, "ident", 5) == 0)
          return static_cast<SongField>(static_cast<int>(SongField::SPB_ident) + slot);
      }
      break;
    }
    case sizeof("unk_unused") - 1:
      if (key_is(s, n, "unk_unused")) return SongField::UnkUnused;
      break;
    case sizeof("title_ascii") - 1:
      if (key_is(s, n, "title_ascii")) return SongField::TitleAscii;
      break;
    case sizeof("texture_load") - 1:
      static_assert(sizeof("texture_list") == sizeof("texture_load"));
      static_assert(sizeof("game_version") == sizeof("texture_load"));
      static_assert(sizeof("other_folder") == sizeof("texture_load"));
      static_assert(sizeof("bga_filename") == sizeof("texture_load"));
      if (key_is(s, n, "texture_load")) return SongField::TextureLoad;
      if (key_is(s, n, "texture_list")) return SongField::TextureList;
      if (key_is(s, n, "game_version")) return SongField::GameVersion;
      if (key_is(s, n, "other_folder")) return SongField::OtherFolder;
      if (key_is(s, n, "bga_filename")) return SongField::BgaFilename;
      break;
    case sizeof("texture_title") - 1:
      static_assert(sizeof("texture_genre") == sizeof("texture_title"));
      static_assert(sizeof("bemani_folder") == sizeof("texture_title"));
      if (key_is(s, n, "texture_title")) return SongField::TextureTitle;
      if (key_is(s, n, "texture_genre")) return SongField::TextureGenre;
      if (key_is(s, n, "bemani_folder")) return SongField::BemaniFolder;
      break;
    case sizeof("texture_artist") - 1:
      if (key_is(s, n, "texture_artist")) return SongField::TextureArtist;
      break;
    case sizeof("iidx_rec_folder") - 1:
      static_assert(sizeof("splittable_diff") == sizeof("iidx_rec_folder"));
      if (key_is(s, n, "splittable_diff")) return SongField::Splittable;
      if (key_is(s, n, "iidx_rec_folder")) return SongField::IidxRec;
      break;
    case sizeof("texture_subtitle") - 1:
      if (key_is(s, n, "texture_subtitle")) return SongField::TextureSubtitle;
      break;
    case sizeof("bemani_rec_folder") - 1:
      if (key_is(s, n, "bemani_rec_folder")) return SongField::BemaniRec;
      break;
    case sizeof("beginner_rec_folder") - 1:
      if (key_is(s, n, "beginner_rec_folder")) return SongField::BeginnerRec;
      break;
    default:
      break;
  }
  return SongField::Unknown;
}

static int i64_to_int(int64_t v) {
  if (v > static_cast<int64_t>(std::numeric_limits<int>::max())) return std::numeric_limits<int>::max();
  if (v < static_cast<int64_t>(std::numeric_limits<int>::min())) return std::numeric_limits<int>::min();
  return static_cast<int>(v);
}

static uint32_t i64_to_u32(int64_t v) {
  if (v < 0) return 0;
  if (static_cast<uint64_t>(v) > 4294967295ull) return 0xffffffffu;
  return static_cast<uint32_t>(v);
}

static int SongData::* const kLevelMem[] = {
    &SongData::SPB_level, &SongData::SPN_level, &SongData::SPH_level, &SongData::SPA_level, &SongData::SPL_level,
    &SongData::DPB_level, &SongData::DPN_level, &SongData::DPH_level, &SongData::DPA_level, &SongData::DPL_level,
};
static int SongData::* const kIdentMem[] = {
    &SongData::SPB_ident, &SongData::SPN_ident, &SongData::SPH_ident, &SongData::SPA_ident, &SongData::SPL_ident,
    &SongData::DPB_ident, &SongData::DPN_ident, &SongData::DPH_ident, &SongData::DPA_ident, &SongData::DPL_ident,
};

static bool read_song_int(JsonReader& jr, int& dest, std::wstring& err) {
  int64_t v = 0;
  if (!jr.read_i64(v, err)) return false;
  dest = i64_to_int(v);
  return true;
}

static bool read_song_u32(JsonReader& jr, uint32_t& dest, std::wstring& err) {
  int64_t v = 0;
  if (!jr.read_i64(v, err)) return false;
  dest = i64_to_u32(v);
  return true;
}

static bool apply_chart_field(JsonReader& jr, SongData& out, SongField field, std::wstring& err) {
  const int raw = static_cast<int>(field);
  const int level0 = static_cast<int>(SongField::SPB_level);
  const int ident0 = static_cast<int>(SongField::SPB_ident);
  if (raw >= level0 && raw < level0 + 10) return read_song_int(jr, out.*kLevelMem[raw - level0], err);
  if (raw >= ident0 && raw < ident0 + 10) return read_song_int(jr, out.*kIdentMem[raw - ident0], err);
  return jr.skip_value(err);
}

static bool parse_song_object(JsonReader& jr, uint32_t version, SongData& out, std::wstring& err) {
  out = SongData{};
  out.afp_data_count = 0;

  if (!jr.expect('{', err)) return false;
  if (jr.consume('}')) return true;
  while (true) {
    char key[32];
    size_t key_len = 0;
    bool too_long = false;
    if (!jr.read_key(key, sizeof(key), key_len, too_long, err)) return false;
    if (!jr.expect(':', err)) return false;

    const SongField field = too_long ? SongField::Unknown : classify_field(key, key_len);
    switch (field) {
      case SongField::SongId:
        if (!read_song_u32(jr, out.song_id, err)) return false;
        break;
      case SongField::Title:
        if (!jr.read_string(out.title, err)) return false;
        break;
      case SongField::TitleAscii:
        if (!jr.read_string(out.title_ascii, err)) return false;
        break;
      case SongField::Genre:
        if (!jr.read_string(out.genre, err)) return false;
        break;
      case SongField::Artist:
        if (!jr.read_string(out.artist, err)) return false;
        break;
      case SongField::Subtitle:
        if (!jr.read_string(out.subtitle, err)) return false;
        break;
      case SongField::TextureTitle:
        if (!read_song_u32(jr, out.texture_title, err)) return false;
        break;
      case SongField::TextureArtist:
        if (!read_song_u32(jr, out.texture_artist, err)) return false;
        break;
      case SongField::TextureGenre:
        if (!read_song_u32(jr, out.texture_genre, err)) return false;
        break;
      case SongField::TextureLoad:
        if (!read_song_u32(jr, out.texture_load, err)) return false;
        break;
      case SongField::TextureList:
        if (!read_song_u32(jr, out.texture_list, err)) return false;
        break;
      case SongField::TextureSubtitle:
        if (!read_song_u32(jr, out.texture_subtitle, err)) return false;
        break;
      case SongField::FontIdx:
        if (!read_song_u32(jr, out.font_idx, err)) return false;
        break;
      case SongField::GameVersion:
        if (!read_song_int(jr, out.game_version, err)) return false;
        break;
      case SongField::OtherFolder:
        if (!read_song_int(jr, out.other_folder, err)) return false;
        break;
      case SongField::BemaniFolder:
        if (!read_song_int(jr, out.bemani_folder, err)) return false;
        break;
      case SongField::BeginnerRec:
        if (!read_song_int(jr, out.beginner_rec_folder, err)) return false;
        break;
      case SongField::IidxRec:
        if (!read_song_int(jr, out.iidx_rec_folder, err)) return false;
        break;
      case SongField::BemaniRec:
        if (!read_song_int(jr, out.bemani_rec_folder, err)) return false;
        break;
      case SongField::Splittable:
        if (!read_song_int(jr, out.splittable_diff, err)) return false;
        break;
      case SongField::UnkUnused:
        if (!read_song_int(jr, out.unk_unused, err)) return false;
        break;
      case SongField::Volume:
        if (!read_song_u32(jr, out.volume, err)) return false;
        break;
      case SongField::BgaFilename:
        if (!jr.read_string(out.bga_filename, err)) return false;
        break;
      case SongField::BgaDelay:
        if (!read_song_int(jr, out.bga_delay, err)) return false;
        break;
      case SongField::AfpFlag:
        if (!read_song_u32(jr, out.afp_flag, err)) return false;
        break;
      case SongField::AfpData: {
        if (!jr.expect('[', err)) return false;
        int idx = 0;
        if (!jr.consume(']')) {
          while (true) {
            jr.skip_ws();
            if (jr.peek('n')) {
              if (!jr.read_null(err)) return false;
              if (idx < 10) out.afp_data[static_cast<size_t>(idx)].clear();
            } else {
              std::string s;
              if (!jr.read_string(s, err)) return false;
              if (idx < 10) out.afp_data[static_cast<size_t>(idx)] = std::move(s);
            }
            ++idx;
            if (jr.consume(']')) break;
            if (!jr.expect(',', err)) return false;
          }
        }
        out.afp_data_count = std::min(idx, 10);
        break;
      }
      default:
        if (!apply_chart_field(jr, out, field, err)) return false;
        break;
    }

    if (jr.consume('}')) break;
    if (!jr.expect(',', err)) return false;
  }

  // Ensure afp_data_count matches expected schema if present.
  if (out.afp_data_count == 0) out.afp_data_count = 0;
  (void)version;
  return true;
}

static bool parse_musicdata_root(const char* json_utf8, size_t json_len, uint32_t& out_version, std::vector<SongData>& out_songs, std::wstring& err) {
  JsonReader jr(json_utf8, json_len);
  out_songs.clear();
  out_version = 0;

  if (!jr.expect('{', err)) return false;
  bool have_ver = false;
  bool have_data = false;
  if (!jr.consume('}')) {
    while (true) {
      char key[32];
      size_t key_len = 0;
      bool too_long = false;
      if (!jr.read_key(key, sizeof(key), key_len, too_long, err)) return false;
      if (!jr.expect(':', err)) return false;

      const SongField field = too_long ? SongField::Unknown : classify_field(key, key_len);
      if (field == SongField::DataVer) {
        if (!read_song_u32(jr, out_version, err)) return false;
        have_ver = true;
      } else if (field == SongField::Data) {
        if (!jr.expect('[', err)) return false;
        have_data = true;
        if (!jr.consume(']')) {
          while (true) {
            SongData song;
            if (!parse_song_object(jr, out_version, song, err)) return false;
            out_songs.emplace_back(std::move(song));
            if (jr.consume(']')) break;
            if (!jr.expect(',', err)) return false;
          }
        }
      } else {
        if (!jr.skip_value(err)) return false;
      }

      if (jr.consume('}')) break;
      if (!jr.expect(',', err)) return false;
    }
  }

  if (!have_ver) {
    err = L"Missing data_ver.";
    return false;
  }
  if (!have_data) {
    err = L"Missing data array.";
    return false;
  }

  jr.skip_ws();
  if (!jr.eof()) {
    err = L"Invalid JSON: trailing content.";
    return false;
  }
  return true;
}
}  // namespace

}  // namespace

bool musicdata_extract_json(const std::vector<uint8_t>& bin, std::string& out_json_utf8, std::wstring& err) {
  err.clear();
  out_json_utf8.clear();
  if (bin.size() < 16) {
    err = L"File too small.";
    return false;
  }
  if (memcmp(bin.data(), "IIDX", 4) != 0) {
    err = L"Not an IIDX musicdata file.";
    return false;
  }
  uint32_t version = static_cast<uint32_t>(bin[4] | (bin[5] << 8u) | (bin[6] << 16u) | (bin[7] << 24u));
  if (!is_supported_version(version)) {
    err = L"Unsupported data_ver.";
    return false;
  }

  BinReader br{bin.data(), bin.size(), 8, true};
  uint16_t available_entries = 0;
  uint32_t total_entries = 0;
  if (version >= 32) {
    available_entries = br.u16le();
    br.u16le();
    total_entries = br.u32le();
  } else {
    available_entries = br.u16le();
    total_entries = br.u32le();
    br.u16le();
  }
  if (!br.ok) {
    err = L"File truncated (header).";
    return false;
  }

  const size_t entry_sz = (version >= 32 && version != 80) ? 4u : 2u;
  if (total_entries > (std::numeric_limits<size_t>::max() / entry_sz)) {
    err = L"Corrupt header (index too large).";
    return false;
  }
  const size_t index_bytes = static_cast<size_t>(total_entries) * entry_sz;
  if (!br.require(index_bytes)) {
    err = L"File truncated (index).";
    return false;
  }
  br.skip(index_bytes);

  const size_t record_bytes = song_record_bytes(version);
  if (available_entries > 0 && record_bytes > (std::numeric_limits<size_t>::max() / available_entries)) {
    err = L"Corrupt header (song count too large).";
    return false;
  }
  const size_t songs_bytes = static_cast<size_t>(available_entries) * record_bytes;
  if (!br.require(songs_bytes)) {
    err = L"File truncated (song data).";
    return false;
  }

  // Pretty JSON is mostly repeated keys, so size it from the song count and cap it if the header is nonsense.
  {
    size_t est = 128 + static_cast<size_t>(available_entries) * 3072u;
    const size_t cap = bin.size() * 8 + 4096;
    if (est > cap) est = cap;
    const size_t floor_sz = bin.size() + bin.size() / 4;
    if (est < floor_sz) est = floor_sz;
    out_json_utf8.reserve(est);
  }

  // Streaming JSON writer (skips DOM construction) while preserving the exact
  // indentation, commas, and key order of the legacy extract output.
  out_json_utf8 += "{\n    \"data_ver\": ";
  append_json_u32(out_json_utf8, version);
  out_json_utf8 += ",\n    \"data\": ";

  if (available_entries == 0) {
    out_json_utf8 += "[]\n";
    out_json_utf8 += '}';
    return true;
  }

  out_json_utf8 += "[\n";
  SongScratch scratch{};
  for (uint32_t i = 0; i < available_entries; ++i) {
    if (!br.require(record_bytes)) {
      err = L"File truncated (song data).";
      out_json_utf8.clear();
      return false;
    }
    append_indent_units(out_json_utf8, 2);
    append_one_song_from_bin_python(out_json_utf8, br, version, scratch, 1);
    if (!br.ok) {
      err = L"File truncated (song data).";
      out_json_utf8.clear();
      return false;
    }
    if (i + 1 < available_entries)
      out_json_utf8 += ",\n";
    else
      out_json_utf8 += '\n';
  }
  append_indent_units(out_json_utf8, 1);
  out_json_utf8 += "]\n";
  out_json_utf8 += '}';
  return true;
}

bool musicdata_create_bin(const char* json_utf8, size_t json_len, std::vector<uint8_t>& out_bin, std::wstring& err) {
  err.clear();
  out_bin.clear();
  if (!json_utf8) {
    err = L"Invalid JSON (expected MusicDataPlugin schema).";
    return false;
  }
  uint32_t version = 0;
  std::vector<SongData> songs;
  if (!parse_musicdata_root(json_utf8, json_len, version, songs, err)) {
    if (err.empty()) err = L"Invalid JSON (expected MusicDataPlugin schema).";
    return false;
  }
  if (!is_supported_version(version)) {
    err = L"Unsupported data_ver.";
    return false;
  }
  BinWriter bw;
  writer_impl_typed(version, bw, songs);
  out_bin = std::move(bw.buf);
  return true;
}

bool musicdata_create_bin(const std::string& json_utf8, std::vector<uint8_t>& out_bin, std::wstring& err) {
  return musicdata_create_bin(json_utf8.data(), json_utf8.size(), out_bin, err);
}
