#include "../drivers/nabu-prime/osm_regs.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

namespace {
const unsigned referenceKhz[] = {
    825600, 940800, 1056000, 1171200, 1286400, 1401600, 1497600,
    1612800, 1708800, 1804800, 1920000, 2016000, 2131200, 2227200,
    2323200, 2419200, 2534400, 2649600, 2745600, 2841600, 2956800};

struct Registers {
  unsigned frequency[osm::kLutMaxEntries] = {};
  unsigned voltage[osm::kLutMaxEntries] = {};
  unsigned enabled = 1;
  unsigned reads = 0;
  unsigned operator()(unsigned offset) {
    ++reads;
    if (offset == osm::kRegEnable) return enabled;
    if (offset >= osm::kRegFreqLut && (offset - osm::kRegFreqLut) % osm::kLutRowSize == 0) {
      unsigned row = (offset - osm::kRegFreqLut) / osm::kLutRowSize;
      assert(row < osm::kLutMaxEntries);
      return frequency[row];
    }
    unsigned row = (offset - osm::kRegVoltLut) / osm::kLutRowSize;
    assert(offset >= osm::kRegVoltLut && row < osm::kLutMaxEntries);
    assert((offset - osm::kRegVoltLut) % osm::kLutRowSize == 0);
    return voltage[row];
  }
};

unsigned Load(Registers& registers, unsigned (&khz)[osm::kLutMaxEntries]) {
  return osm::LoadFrequencyTable([&](unsigned offset) { return registers(offset); }, khz);
}

Registers Reference(unsigned count = 21) {
  Registers registers;
  for (unsigned i = 0; i < osm::kLutMaxEntries; ++i) {
    unsigned index = i < count ? i : count - 1;
    registers.frequency[i] = 0x40040000u | (i << 8) | (referenceKhz[index] / osm::kXoKhz);
    registers.voltage[i] = 900;
  }
  return registers;
}

void TestSnapshot(const char* path) {
  FILE* file = fopen(path, "r");
  assert(file);
  Registers registers;
  char line[256];
  unsigned rows = 0;
  while (fgets(line, sizeof(line), file)) {
    unsigned index, frequency, voltage;
    if (sscanf(line, "%u %x %x", &index, &frequency, &voltage) != 3) continue;
    assert(index == rows && rows < osm::kLutMaxEntries);
    registers.frequency[rows] = frequency;
    registers.voltage[rows++] = voltage;
  }
  fclose(file);
  assert(rows == osm::kLutMaxEntries);
  unsigned khz[osm::kLutMaxEntries] = {};
  unsigned count = Load(registers, khz);
  assert(count == 20 && khz[count - 1] == 2841600);
  for (unsigned i = 0; i < count; ++i) assert(khz[i] == referenceKhz[i]);
  assert(registers.reads == 43);  // Enable plus 21 frequency/voltage pairs.
  for (unsigned i = 0; i < 21; ++i) {
    unsigned level = osm::LevelAtOrBelow(khz, count, referenceKhz[i]);
    assert(level < count && khz[level] <= referenceKhz[i]);
    assert(level == (i < count ? i : count - 1));
  }
}
}

int main(int argc, char** argv) {
  TestSnapshot(argc > 1 ? argv[1] : "tests/fixtures/nabu-2841-lut.txt");
  unsigned khz[osm::kLutMaxEntries] = {};
  Registers registers = Reference();
  assert(Load(registers, khz) == 21 && khz[20] == 2956800);
  for (unsigned i = 0; i < 21; ++i) {
    assert(osm::LevelAtOrBelow(khz, 21, referenceKhz[i]) == i);
  }

  // This intermediate frequency is absent from the upstream reference array.
  registers = Reference(20);
  registers.frequency[8] = (registers.frequency[8] & ~0xffu) | 88u;
  assert(Load(registers, khz) == 20 && khz[8] == 1689600);
  for (unsigned ceiling = 825600; ceiling <= 2956800; ceiling += 19200) {
    unsigned level = osm::LevelAtOrBelow(khz, 20, ceiling);
    assert(level < 20 && khz[level] <= ceiling);
    assert(level == 19 || khz[level + 1] > ceiling);
  }

  registers = Reference(1);
  assert(Load(registers, khz) == 1 && khz[0] == 825600);
  registers = Reference(); registers.enabled = 0;
  assert(Load(registers, khz) == 0 && registers.reads == 1);
  registers = Reference(); registers.frequency[0] = 0;
  assert(Load(registers, khz) == 0);
  registers = Reference(); registers.frequency[0]++;
  assert(Load(registers, khz) == 0);
  registers = Reference(); registers.frequency[8] = registers.frequency[6];
  assert(Load(registers, khz) == 0);
  registers = Reference(); registers.frequency[20] = 0x400000ff;
  assert(Load(registers, khz) == 0);

  // Maximum bounded table, with and without a final duplicate terminator.
  for (unsigned i = 0; i < osm::kLutMaxEntries; ++i) {
    registers.frequency[i] = 0x40000000 | (43 + i);
  }
  assert(Load(registers, khz) == 0);
  registers.frequency[39] = registers.frequency[38];
  assert(Load(registers, khz) == 39 && khz[38] == (43 + 38) * 19200);
  puts("PASS: real 20-OPP snapshot, original 21-OPP table, dynamic intermediate OPP, thermal ceilings, malformed tables, and table bounds");
  return 0;
}
