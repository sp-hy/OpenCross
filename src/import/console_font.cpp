#include "import/console_font.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <system_error>

namespace oc::import {

namespace {

#include "import/console_font_chars.inc"

// Outline shapes and their filled forms.
constexpr char32_t kShapePairs[][2] = {{0x25A1, 0x25A0}, {0x25CB, 0x25CF}, {0x25C7, 0x25C6}, {0x25B3, 0x25B2}, {0x25BD, 0x25BC}, {0x2606, 0x2605}};
// Ink of a character cell (pixels of 240), and of an outline shape at least.
constexpr int kMinInk = 2, kMaxInk = 200, kMinOutlineInk = 12;
constexpr uintmax_t kMinFileSize = 512 * 1024, kMaxFileSize = 1024 * 1024;

std::mutex g_mutex;
std::shared_ptr<const ConsoleFont> g_font;

int ink(const u8* cell) {
  int count = 0;
  for (int i = 0; i < ConsoleFont::kCellBytes; ++i) count += std::popcount(cell[i]);
  return count;
}

bool valid_at(ByteSpan image, size_t base) {
  const size_t size = static_cast<size_t>(kConsoleFontCells) * ConsoleFont::kCellBytes;
  if (base + size > image.size()) return false;
  const u8* table = image.data() + base;
  auto cell = [&](int index) { return table + static_cast<size_t>(index) * ConsoleFont::kCellBytes; };
  if (ink(cell(0)) != 0) return false;
  for (const auto& pair : kShapePairs) {
    const u8* outline = cell(ConsoleFont::cell_of(pair[0]));
    const u8* filled = cell(ConsoleFont::cell_of(pair[1]));
    for (int i = 0; i < ConsoleFont::kCellBytes; ++i)
      if (outline[i] & ~filled[i]) return false;
    const int outline_ink = ink(outline);
    if (outline_ink < kMinOutlineInk || ink(filled) * 2 <= outline_ink * 3) return false;
  }
  for (int index = 1; index < kConsoleFontCells; ++index) {
    const int count = ink(cell(index));
    if (count < kMinInk || count > kMaxInk || std::memcmp(cell(index), cell(index - 1), ConsoleFont::kCellBytes) == 0) return false;
  }
  return true;
}

bool font_file_name(const std::filesystem::path& file) {
  std::string ext = file.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](char ch) { return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch); });
  return ext == ".bin" || ext == ".rom";
}

}  // namespace

int ConsoleFont::cells() { return kConsoleFontCells; }

int ConsoleFont::cell_of(char32_t cp) {
  const auto* end = std::end(kConsoleFontChars);
  const auto* it = std::lower_bound(std::begin(kConsoleFontChars), end, cp, [](const auto& entry, char32_t value) { return entry.cp < value; });
  return it != end && it->cp == cp ? it->cell : -1;
}

// The space cell lies in a run of zero bytes and the next cell has ink, so the space starts at most 59 bytes before the run's end.
std::optional<size_t> ConsoleFont::find(ByteSpan image) {
  size_t at = 0;
  while (at < image.size()) {
    if (image[at] != 0) {
      ++at;
      continue;
    }
    size_t end = at;
    while (end < image.size() && image[end] == 0) ++end;
    if (end - at >= static_cast<size_t>(kCellBytes)) {
      const size_t first = std::max(at, end + 1 > 2 * static_cast<size_t>(kCellBytes) ? end + 1 - 2 * static_cast<size_t>(kCellBytes) : 0);
      for (size_t base = first; base + kCellBytes <= end; ++base)
        if (valid_at(image, base)) return base;
    }
    at = end;
  }
  return std::nullopt;
}

std::shared_ptr<const ConsoleFont> ConsoleFont::from_image(ByteSpan image) {
  const auto base = find(image);
  if (!base) return nullptr;
  auto font = std::make_shared<ConsoleFont>();
  const auto begin = image.begin() + static_cast<std::ptrdiff_t>(*base);
  font->bits_.assign(begin, begin + static_cast<std::ptrdiff_t>(kConsoleFontCells) * kCellBytes);
  return font;
}

bool ConsoleFont::pixel(int cell, int x, int y) const {
  if (cell < 0 || cell >= kConsoleFontCells || x < 0 || y < 0 || x >= kCellW || y >= kCellH) return false;
  return bits_[static_cast<size_t>(cell) * kCellBytes + static_cast<size_t>(2 * y + x / 8)] & (0x80 >> (x & 7));
}

std::shared_ptr<const ConsoleFont> find_console_font(const std::vector<std::filesystem::path>& dirs, std::filesystem::path* file) {
  for (const std::filesystem::path& dir : dirs) {
    std::error_code error;
    std::vector<std::filesystem::path> candidates;
    for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
      std::error_code size_error;
      if (!entry.is_regular_file(size_error) || !font_file_name(entry.path())) continue;
      const uintmax_t size = entry.file_size(size_error);
      if (!size_error && size >= kMinFileSize && size <= kMaxFileSize) candidates.push_back(entry.path());
    }
    std::sort(candidates.begin(), candidates.end());
    for (const std::filesystem::path& candidate : candidates) {
      std::ifstream in(candidate, std::ios::binary);
      const Bytes bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      if (auto font = ConsoleFont::from_image(bytes)) {
        if (file) *file = candidate;
        return font;
      }
    }
  }
  return nullptr;
}

void set_console_font(std::shared_ptr<const ConsoleFont> font) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_font = std::move(font);
}

std::shared_ptr<const ConsoleFont> console_font() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_font;
}

}  // namespace oc::import
