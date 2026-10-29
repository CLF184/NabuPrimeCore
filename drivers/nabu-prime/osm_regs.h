#pragma once

namespace osm {

constexpr unsigned long long kDomainBase[3] = {0x18323000ull, 0x18325800ull, 0x18327800ull};

constexpr unsigned long long kL3Base = 0x18321000ull;
constexpr unsigned kDomainSize = 0x1400;

constexpr unsigned kRegEnable = 0x000;
constexpr unsigned kRegDcvsCtrl = 0x0bc;
constexpr unsigned kRegFreqLut = 0x110;
constexpr unsigned kRegVoltLut = 0x114;
constexpr unsigned kRegCurrentVote = 0x704;
constexpr unsigned kRegPerfState = 0x920;
constexpr unsigned kLutRowSize = 32;
constexpr unsigned kLutMaxEntries = 40;

constexpr unsigned kXoKhz = 19200;
constexpr unsigned kHwDivKhz = 300000;

struct LutRow {
  unsigned src;
  unsigned lval;
  unsigned coreCount;
  unsigned millivolts;
  unsigned khz;
};

inline LutRow DecodeLut(unsigned freqWord, unsigned voltWord) {
  LutRow r;
  r.src = (freqWord >> 30) & 0x3;
  r.lval = freqWord & 0xff;
  r.coreCount = (freqWord >> 16) & 0x7;
  r.millivolts = voltWord & 0xfff;
  r.khz = r.src ? r.lval * kXoKhz : kHwDivKhz;
  return r;
}

inline unsigned CurrentVoteKhz(unsigned word) { return (word & 0x3ff) * kXoKhz; }

// A repeated frequency terminates the firmware's OSM table. Keep the SD860
// operating range as a guard, without requiring particular intermediate OPPs.
template <typename ReadWord>
unsigned LoadFrequencyTable(ReadWord read, unsigned (&khz)[kLutMaxEntries]) {
  if (!(read(kRegEnable) & 1u)) return 0;
  unsigned previous = 0;
  for (unsigned i = 0; i < kLutMaxEntries; ++i) {
    LutRow row = DecodeLut(read(kRegFreqLut + i * kLutRowSize),
                           read(kRegVoltLut + i * kLutRowSize));
    if (row.khz < 825600 || row.khz > 2956800) return 0;
    if (i == 0 && row.khz != 825600) return 0;
    if (i > 0 && row.khz == previous) return i;
    if (row.khz <= previous) return 0;
    khz[i] = row.khz;
    previous = row.khz;
  }
  return 0;  // No terminator within the hardware table.
}

inline unsigned LevelAtOrBelow(const unsigned* khz, unsigned count, unsigned ceilingKhz) {
  unsigned level = 0;
  for (unsigned i = 1; i < count && khz[i] <= ceilingKhz; ++i) level = i;
  return level;
}

}
