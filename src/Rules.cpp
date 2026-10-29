#include "Rules.h"
#include "Config.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace rules {
static bool asciiAlnum(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}
static char lower(char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
static bool reserved(const std::string& s) {
  std::string base = s.substr(0, s.find('.'));
  for (char& c : base) c = lower(c);
  if (base == "con" || base == "prn" || base == "aux" || base == "nul") return true;
  return base.size() == 4 && (base.substr(0, 3) == "com" || base.substr(0, 3) == "lpt") &&
         base[3] >= '1' && base[3] <= '9';
}
bool utf8(const std::string& s, size_t* utf16Units) {
  size_t units = 0;
  for (size_t i = 0; i < s.size();) {
    const uint8_t c = static_cast<uint8_t>(s[i++]);
    uint32_t cp; size_t n; uint32_t minimum;
    if (c < 0x80) { cp = c; n = 0; minimum = 0; }
    else if (c >= 0xc2 && c <= 0xdf) { cp = c & 0x1f; n = 1; minimum = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { cp = c & 0x0f; n = 2; minimum = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { cp = c & 7; n = 3; minimum = 0x10000; }
    else return false;
    if (i + n > s.size()) return false;
    while (n--) {
      uint8_t b = static_cast<uint8_t>(s[i++]);
      if ((b & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (b & 0x3f);
    }
    if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    // Keine C0-/C1-Steuerzeichen, DEL oder Unicode-Zeilentrenner.
    if (cp < 0x20 || (cp >= 0x7f && cp <= 0x9f) || cp == 0x2028 || cp == 0x2029) return false;
    units += cp >= 0x10000 ? 2 : 1;
  }
  if (utf16Units) *utf16Units = units;
  return true;
}
bool mp3Extension(const std::string& f) {
  return f.size() > 4 && f[f.size()-4] == '.' && lower(f[f.size()-3]) == 'm' &&
         lower(f[f.size()-2]) == 'p' && f.back() == '3';
}
int naturalCompare(const std::string& left, const std::string& right) {
  size_t li = 0, ri = 0;
  while (li < left.size() && ri < right.size()) {
    const unsigned char l = static_cast<unsigned char>(left[li]);
    const unsigned char r = static_cast<unsigned char>(right[ri]);
    const bool lDigit = l >= '0' && l <= '9';
    const bool rDigit = r >= '0' && r <= '9';
    if (lDigit && rDigit) {
      const size_t lStart = li, rStart = ri;
      while (li < left.size() && left[li] >= '0' && left[li] <= '9') ++li;
      while (ri < right.size() && right[ri] >= '0' && right[ri] <= '9') ++ri;
      size_t lNumber = lStart, rNumber = rStart;
      while (lNumber < li && left[lNumber] == '0') ++lNumber;
      while (rNumber < ri && right[rNumber] == '0') ++rNumber;
      const size_t lDigits = li-lNumber, rDigits = ri-rNumber;
      if (lDigits != rDigits) return lDigits < rDigits ? -1 : 1;
      for (size_t i = 0; i < lDigits; ++i) {
        if (left[lNumber+i] != right[rNumber+i]) return left[lNumber+i] < right[rNumber+i] ? -1 : 1;
      }
      // Gleicher Zahlenwert: die kuerzere Schreibweise zuerst (1 vor 01).
      const size_t lRun = li-lStart, rRun = ri-rStart;
      if (lRun != rRun) return lRun < rRun ? -1 : 1;
      continue;
    }
    if (l != r) return l < r ? -1 : 1;
    ++li; ++ri;
  }
  if (li == left.size() && ri == right.size()) return 0;
  return li == left.size() ? -1 : 1;
}
bool motionFile(const std::string& f) {
  if (!mp3Extension(f) || f.size()-4 > cfg::MAX_TITLE_LEN || reserved(f)) return false;
  for (size_t i = 0; i < f.size()-4; ++i) if (!asciiAlnum(f[i])) return false;
  return true;
}
bool playlistName(const std::string& s) {
  if (s.empty() || s.size() > cfg::MAX_PLAYLIST_LEN || s.front() == ' ' || s.back() == ' ' ||
      s.front() == '.' || s.back() == '.' || reserved(s)) return false;
  for (unsigned char c : s)
    if (!asciiAlnum(c) && std::string(" _-().,").find(c) == std::string::npos) return false;
  return true;
}
bool musicFile(const std::string& f) {
  size_t units;
  if (!mp3Extension(f) || f.size() > cfg::MAX_FILENAME_BYTES || f.front() == '.' ||
      f.front() == ' ' || reserved(f) || !utf8(f, &units) || units > cfg::MAX_FILENAME_UTF16) return false;
  // Auch der Titel direkt vor .mp3 soll nicht auf Punkt/Leerzeichen enden.
  if (f[f.size()-5] == ' ' || f[f.size()-5] == '.') return false;
  for (unsigned char c : f)
    if (std::string("/\\:*?\"<>|").find(c) != std::string::npos) return false;
  return true;
}
bool btName(const std::string& s) {
  if (s.empty() || s.size() > 32 || s.front() == ' ' || s.back() == ' ') return false;
  // BT-Name ist nicht der Playlistname; Unicode darf hier verwendet werden.
  return utf8(s);
}
bool weight(const std::string& s, double& out) {
  if (s.empty() || s.size() > 20) return false;
  bool digit = false;
  for (char c : s) { if (c >= '0' && c <= '9') digit = true; else if (c != '.') return false; }
  if (!digit) return false;
  char* end = nullptr;
  double v = std::strtod(s.c_str(), &end);
  if (*end || !std::isfinite(v) || v < 0 || v > cfg::MAX_WEIGHT) return false;
  out = v; return true;
}
bool unsignedNumber(const std::string& s, uint32_t max, uint32_t& out) {
  if (s.empty() || s.size() > 10) return false;
  uint64_t n = 0;
  for (char c : s) { if (c < '0' || c > '9') return false; n = n*10 + c-'0'; if (n > max) return false; }
  out = static_cast<uint32_t>(n); return true;
}
std::string html(const std::string& s) {
  std::string r; r.reserve(s.size()+16);
  for (char c : s) {
    switch(c) {
      case '&': r += "&amp;"; break; case '<': r += "&lt;"; break;
      case '>': r += "&gt;"; break; case '"': r += "&quot;"; break;
      case '\'': r += "&#39;"; break; default: r += c;
    }
  }
  return r;
}
std::string json(const std::string& s) {
  const char* hex = "0123456789abcdef"; std::string r = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { r += '\\'; r += c; }
    else if (c < 32) { r += "\\u00"; r += hex[c>>4]; r += hex[c&15]; }
    else r += c;
  }
  return r + '"';
}
std::string url(const std::string& s) {
  const char* hex = "0123456789ABCDEF"; std::string r;
  for (unsigned char c : s) {
    if (asciiAlnum(c) || c == '-' || c == '_' || c == '.' || c == '~') r += c;
    else { r += '%'; r += hex[c>>4]; r += hex[c&15]; }
  }
  return r;
}
int weightedPick(const std::vector<double>& w, int last, uint32_t random) {
  size_t enabled = 0;
  for (double v : w) if (v > 0 && std::isfinite(v)) ++enabled;
  if (!enabled) return -1;
  double sum = 0;
  for (size_t i = 0; i < w.size(); ++i)
    if (w[i] > 0 && std::isfinite(w[i]) && (enabled == 1 || static_cast<int>(i) != last)) sum += w[i];
  double target = (static_cast<double>(random) / 4294967296.0) * sum;
  int fallback = -1;
  for (size_t i = 0; i < w.size(); ++i) {
    if (!(w[i] > 0) || !std::isfinite(w[i]) || (enabled > 1 && static_cast<int>(i) == last)) continue;
    fallback = static_cast<int>(i); target -= w[i]; if (target < 0) return fallback;
  }
  return fallback;
}
uint32_t below(uint32_t n, uint32_t (*rng)()) {
  if (n <= 1) return 0;
  const uint32_t threshold = static_cast<uint32_t>(-n) % n;
  uint32_t r; do { r = rng(); } while (r < threshold);
  return r % n;
}
void order(std::vector<uint16_t>& out, size_t count, bool shuffle, int previous, uint32_t (*rng)()) {
  out.resize(count);
  for (size_t i = 0; i < count; ++i) out[i] = static_cast<uint16_t>(i);
  if (!shuffle) return;
  for (size_t i = count; i > 1; --i) std::swap(out[i-1], out[below(i, rng)]);
  if (count > 1 && static_cast<int>(out[0]) == previous) std::swap(out[0], out[1+below(count-1, rng)]);
}
}
