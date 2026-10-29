#include <ntddk.h>
#include <ntstrsafe.h>

#include "osm_regs.h"

extern "C" NTSYSAPI NTSTATUS NTAPI ZwQuerySystemInformation(ULONG cls, PVOID info, ULONG len, PULONG ret);
extern "C" NTSYSAPI NTSTATUS NTAPI ZwWaitForSingleObject(HANDLE handle, BOOLEAN alertable, PLARGE_INTEGER timeout);

namespace {

constexpr ULONG kPeriodMs = 50;
constexpr ULONG kJumpBusy = 85;
constexpr ULONG kDownWindows = 3;
constexpr ULONG kThermalTopLevel = 20;
constexpr ULONG kTag = 'PNon';

// Preserve the original temperature/frequency limits, independently of the
// number and spacing of OPPs provided by this unit's firmware.
constexpr ULONG kThermalKhz[kThermalTopLevel + 1] = {
    825600,  940800,  1056000, 1171200, 1286400, 1401600, 1497600,
    1612800, 1708800, 1804800, 1920000, 2016000, 2131200, 2227200,
    2323200, 2419200, 2534400, 2649600, 2745600, 2841600, 2956800};
constexpr ULONG kSystemProcessorPerformanceInformation = 8;
constexpr ULONG kPrimeIndex = 7;

constexpr ULONGLONG kTsens0Tm = 0x0c263000ull;
constexpr ULONG kTsensSize = 0x200;
constexpr ULONG kSensorTop = 10;
constexpr ULONG kSensorBottom = 14;

struct CapPoint {
  LONG deciC;
  LONG level;
};
constexpr CapPoint kCapCurve[] = {{865, 20}, {925, 8}, {950, 0}};
constexpr LONG kCapZeroDeciC = 950;
constexpr LONG kCapGuardDeciC = 30;
constexpr LONG kCapRiseMarginDeciC = 10;
constexpr ULONG kCapRiseMs = 2000;
constexpr LONG kTempAvgShift = 5;
constexpr ULONG kCapUnknownTemp = 12;

struct CpuTimes {
  LARGE_INTEGER idle, kernel, user, dpc, interrupt;
  ULONG interruptCount;
};

volatile ULONG* gOsm;
volatile ULONG* gTsens;
ULONG gOriginal;
ULONG gTopLevel;
ULONG gMaxLevel = MAXULONG;
ULONG gLogIntervalMs;
unsigned gLutKhz[osm::kLutMaxEntries];
ULONG gProcessors;
CpuTimes* gTimes;
HANDLE gThread;
PKTHREAD gThreadObject;
PVOID gDisplayCallback;
KEVENT gWake;
volatile LONG gStop;
volatile LONG gDisplayOn = 1;

LONG gTempAvgScaled;
bool gTempAvgValid;
LONGLONG gCapChangedMs;

ULONG OsmRead(ULONG off) { return READ_REGISTER_ULONG(gOsm + off / 4); }

void SetPrime(ULONG level) {
  if (level > gTopLevel) level = gTopLevel;
  WRITE_REGISTER_ULONG(gOsm + osm::kRegPerfState / 4, level);
}

ULONG ReadPrime() {
  ULONG level = OsmRead(osm::kRegPerfState);
  return level <= gTopLevel ? level : gTopLevel;
}

bool ReadSensorDeciC(ULONG sensor, LONG* out) {
  if (!gTsens) return false;
  ULONG off = (0xa0 + 4 * sensor) / 4;
  ULONG prev = 0;
  for (int attempt = 0; attempt < 3; ++attempt) {
    ULONG raw = READ_REGISTER_ULONG(gTsens + off);
    ULONG code = raw & 0xfff;
    if ((raw & (1u << 21)) || (attempt > 0 && code == prev)) {
      LONG v = static_cast<LONG>(code);
      if (v & 0x800) v -= 0x1000;
      if (v < -200 || v > 1500) return false;
      *out = v;
      return true;
    }
    prev = code;
  }
  return false;
}

ULONG ThermalLevel(ULONG referenceLevel) {
  return osm::LevelAtOrBelow(gLutKhz, gTopLevel + 1, kThermalKhz[referenceLevel]);
}

ULONG CapForDeciC(LONG t) {
  if (t <= kCapCurve[0].deciC) return gTopLevel;
  for (ULONG i = 1; i < RTL_NUMBER_OF(kCapCurve); ++i) {
    const CapPoint& a = kCapCurve[i - 1];
    const CapPoint& b = kCapCurve[i];
    if (t < b.deciC) {

      LONG span = b.deciC - a.deciC;
      ULONG referenceLevel = static_cast<ULONG>(
          (a.level * span + (b.level - a.level) * (t - a.deciC)) / span);
      return ThermalLevel(referenceLevel);
    }
  }
  return 0;
}

ULONG CapFor(LONG avg, LONG hot, LONG margin) {
  ULONG byAvg = CapForDeciC(avg + margin);
  ULONG byRaw = CapForDeciC(hot - kCapGuardDeciC + margin);
  return byAvg < byRaw ? byAvg : byRaw;
}

void ResetThermalAverage() { gTempAvgValid = false; }

ULONG UpdateThermalCap(ULONG prevCap, LONGLONG nowMs, LONG* topOut, LONG* botOut, LONG* avgOut) {
  LONG top = 0, bot = 0;
  bool okTop = ReadSensorDeciC(kSensorTop, &top);
  bool okBot = ReadSensorDeciC(kSensorBottom, &bot);
  *topOut = okTop ? top : -1000;
  *botOut = okBot ? bot : -1000;
  ULONG cap;
  if (!okTop && !okBot) {
    *avgOut = -1000;
    cap = ThermalLevel(kCapUnknownTemp);
  } else {
    LONG hot = (okTop && okBot) ? (top > bot ? top : bot) : (okTop ? top : bot);
    if (!gTempAvgValid) {
      gTempAvgScaled = hot * (1 << kTempAvgShift);
      gTempAvgValid = true;
    } else {
      gTempAvgScaled += hot - (gTempAvgScaled >> kTempAvgShift);
    }
    *avgOut = gTempAvgScaled >> kTempAvgShift;
    cap = CapFor(*avgOut, hot, 0);

    if (cap > prevCap) {
      ULONG withMargin = CapFor(*avgOut, hot, kCapRiseMarginDeciC);
      cap = withMargin > prevCap ? withMargin : prevCap;
    }
    if (hot >= kCapZeroDeciC) cap = 0;
  }
  if (cap > gMaxLevel) cap = gMaxLevel;
  if (cap > prevCap) {
    if (nowMs - gCapChangedMs < static_cast<LONGLONG>(kCapRiseMs)) return prevCap;
    cap = prevCap + 1;
  }
  if (cap != prevCap) gCapChangedMs = nowMs;
  return cap;
}

ULONG PickLevel(ULONG busyPct, ULONG current, ULONG cap) {
  if (cap == 0 || busyPct == 0) return 0;
  if (busyPct >= kJumpBusy) return cap;
  ULONGLONG targetKhz = static_cast<ULONGLONG>(busyPct) * gLutKhz[current] * 5 / 400;
  for (ULONG lvl = 0; lvl < cap; ++lvl) {
    if (gLutKhz[lvl] >= targetKhz) return lvl;
  }
  return cap;
}

ULONG SampleBusyPct(CpuTimes* prev, LONGLONG wall100ns) {
  NTSTATUS s = ZwQuerySystemInformation(kSystemProcessorPerformanceInformation, gTimes,
                                        sizeof(CpuTimes) * gProcessors, nullptr);
  if (!NT_SUCCESS(s)) return 0;
  const CpuTimes& cur = gTimes[kPrimeIndex];
  LONGLONG dIdle = cur.idle.QuadPart - prev->idle.QuadPart;
  LONGLONG dKernel = cur.kernel.QuadPart - prev->kernel.QuadPart;
  LONGLONG dUser = cur.user.QuadPart - prev->user.QuadPart;
  *prev = cur;
  LONGLONG dBusy = (dKernel > dIdle ? dKernel - dIdle : 0) + (dUser > 0 ? dUser : 0);
  LONGLONG dTotal = (dKernel > 0 ? dKernel : 0) + (dUser > 0 ? dUser : 0);
  if (dBusy <= 0 || dTotal <= 0) return 0;
  LONGLONG denom = (wall100ns > 0 && dTotal > wall100ns * 3 / 2) ? wall100ns : dTotal;
  if (denom <= 0) return 0;
  if (dBusy >= denom) return 100;
  return static_cast<ULONG>((dBusy * 100) / denom);
}

class LogFile {
 public:
  NTSTATUS Open() {
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"\\??\\C:\\nabu\\osm\\prime.log");
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, nullptr, nullptr);
    IO_STATUS_BLOCK io;
    return ZwCreateFile(&h_, GENERIC_WRITE | SYNCHRONIZE, &oa, &io, nullptr, FILE_ATTRIBUTE_NORMAL,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OVERWRITE_IF,
                        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, nullptr, 0);
  }
  void Line(const char* fmt, ...) {
    if (!h_) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    NTSTATUS s = RtlStringCbVPrintfA(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (!NT_SUCCESS(s) && s != STATUS_BUFFER_OVERFLOW) return;
    size_t len = 0;
    RtlStringCbLengthA(buf, sizeof(buf), &len);
    buf[len++] = '\r';
    buf[len++] = '\n';
    IO_STATUS_BLOCK io;
    ZwWriteFile(h_, nullptr, nullptr, nullptr, &io, buf, static_cast<ULONG>(len), nullptr, nullptr);
  }
  ~LogFile() {
    if (h_) ZwClose(h_);
  }

 private:
  HANDLE h_ = nullptr;
};

constexpr GUID kGuidConsoleDisplayState = {
    0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};

NTSTATUS NTAPI DisplayCallback(LPCGUID setting, PVOID value, ULONG len, PVOID) {
  if (RtlCompareMemory(setting, &kGuidConsoleDisplayState, sizeof(GUID)) == sizeof(GUID) &&
      value && len >= sizeof(ULONG)) {
    ULONG state = *static_cast<const ULONG*>(value);
    InterlockedExchange(&gDisplayOn, state != 0 ? 1 : 0);
    KeSetEvent(&gWake, IO_NO_INCREMENT, FALSE);
  }
  return STATUS_SUCCESS;
}

void ReadParameters(PUNICODE_STRING regPath) {
  if (!regPath || !regPath->Buffer) return;
  OBJECT_ATTRIBUTES oa;
  InitializeObjectAttributes(&oa, regPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, nullptr, nullptr);
  HANDLE root = nullptr;
  if (!NT_SUCCESS(ZwOpenKey(&root, KEY_READ, &oa))) return;
  UNICODE_STRING subName;
  RtlInitUnicodeString(&subName, L"Parameters");
  InitializeObjectAttributes(&oa, &subName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, root, nullptr);
  HANDLE params = nullptr;
  if (NT_SUCCESS(ZwOpenKey(&params, KEY_READ, &oa))) {
    auto readDword = [&](PCWSTR valueName, ULONG* out) {
      UNICODE_STRING vn;
      RtlInitUnicodeString(&vn, valueName);
      UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)] = {};
      ULONG res = 0;
      if (NT_SUCCESS(ZwQueryValueKey(params, &vn, KeyValuePartialInformation, buf, sizeof(buf), &res))) {
        auto* info = reinterpret_cast<KEY_VALUE_PARTIAL_INFORMATION*>(buf);
        if (info->Type == REG_DWORD && info->DataLength == sizeof(ULONG)) {
          *out = *reinterpret_cast<const ULONG*>(info->Data);
        }
      }
    };
    readDword(L"MaxLevel", &gMaxLevel);
    readDword(L"LogIntervalMs", &gLogIntervalMs);
    ZwClose(params);
  }
  ZwClose(root);
}

void Runner(PVOID) {
  LARGE_INTEGER qpcFreq;
  LONGLONG startQpc = KeQueryPerformanceCounter(&qpcFreq).QuadPart;
  LONGLONG perMs = qpcFreq.QuadPart / 1000;
  if (perMs <= 0) perMs = 1;

  LogFile log;
  if (gLogIntervalMs > 0 && NT_SUCCESS(log.Open())) {
    log.Line("NabuPrime qpc_freq %lld start_qpc %lld original_level %u max_level %u period_ms %u",
             qpcFreq.QuadPart, startQpc, gOriginal, gMaxLevel, kPeriodMs);
    log.Line("LUT levels %u min_khz %u max_khz %u",
             gTopLevel + 1, gLutKhz[0], gLutKhz[gTopLevel]);
  }

  ZwQuerySystemInformation(kSystemProcessorPerformanceInformation, gTimes,
                           sizeof(CpuTimes) * gProcessors, nullptr);
  CpuTimes prevTimes = gTimes[kPrimeIndex];
  LONGLONG prevQpc = KeQueryPerformanceCounter(nullptr).QuadPart;
  LONGLONG lastLogMs = -static_cast<LONGLONG>(gLogIntervalMs);

  ULONG thermalCap = gMaxLevel;
  ULONG downCount = 0;

  LARGE_INTEGER delay;
  delay.QuadPart = -static_cast<LONGLONG>(kPeriodMs) * 10000;

  while (!gStop) {
    if (!gDisplayOn) {
      ULONG before = ReadPrime();
      if (before != 0) SetPrime(0);
      downCount = 0;
      if (gLogIntervalMs > 0) {
        LONGLONG ms = (KeQueryPerformanceCounter(nullptr).QuadPart - startQpc) / perMs;
        log.Line("P %lld parked from %u hw %u", ms, before, ReadPrime());
        lastLogMs = ms;
      }
      KeClearEvent(&gWake);
      if (gStop || gDisplayOn) continue;
      KeWaitForSingleObject(&gWake, Executive, KernelMode, FALSE, nullptr);
      ZwQuerySystemInformation(kSystemProcessorPerformanceInformation, gTimes,
                               sizeof(CpuTimes) * gProcessors, nullptr);
      prevTimes = gTimes[kPrimeIndex];
      prevQpc = KeQueryPerformanceCounter(nullptr).QuadPart;
      ResetThermalAverage();
      if (gLogIntervalMs > 0) {
        LONGLONG ms = (prevQpc - startQpc) / perMs;
        log.Line("R %lld resumed display %d hw %u", ms, gDisplayOn ? 1 : 0, ReadPrime());
        lastLogMs = ms;
      }
      continue;
    }

    KeWaitForSingleObject(&gWake, Executive, KernelMode, FALSE, &delay);
    KeClearEvent(&gWake);
    if (gStop) break;
    if (!gDisplayOn) continue;

    LONGLONG nowQpc = KeQueryPerformanceCounter(nullptr).QuadPart;
    LONGLONG wall100ns = ((nowQpc - prevQpc) * 10000000LL) / qpcFreq.QuadPart;
    prevQpc = nowQpc;

    ULONG current = ReadPrime();
    ULONG busy = SampleBusyPct(&prevTimes, wall100ns);
    LONG topDeciC = 0, botDeciC = 0, avgDeciC = 0;
    thermalCap = UpdateThermalCap(thermalCap, (nowQpc - startQpc) / perMs, &topDeciC, &botDeciC,
                                  &avgDeciC);
    ULONG want = PickLevel(busy, current, thermalCap);

    ULONG next = current;
    if (want >= current) {
      next = want;
      downCount = 0;
    } else if (current > thermalCap) {
      next = thermalCap;
      downCount = 0;
    } else {
      if (++downCount >= kDownWindows) {
        next = want;
        downCount = 0;
      }
    }
    if (next > thermalCap) next = thermalCap;

    if (next != current) SetPrime(next);

    if (gLogIntervalMs > 0) {
      LONGLONG ms = (nowQpc - startQpc) / perMs;
      if (ms - lastLogMs >= static_cast<LONGLONG>(gLogIntervalMs)) {
        ULONG hw = ReadPrime();
        log.Line("S %lld busy %u from %u level %u hw %u khz %u cap %u t_top %ld t_bot %ld t_avg %ld",
                 ms, busy, current, next, hw, gLutKhz[hw], thermalCap, topDeciC, botDeciC, avgDeciC);
        lastLogMs = ms;
      }
    }
  }

  SetPrime(gOriginal);
  if (gLogIntervalMs > 0) {
    LONGLONG ms = (KeQueryPerformanceCounter(nullptr).QuadPart - startQpc) / perMs;
    log.Line("end %lld restored_level %u hw %u", ms, gOriginal, ReadPrime());
  }
}

void Cleanup() {
  if (gDisplayCallback) {
    PoUnregisterPowerSettingCallback(gDisplayCallback);
    gDisplayCallback = nullptr;
  }
  if (gTimes) {
    ExFreePoolWithTag(gTimes, kTag);
    gTimes = nullptr;
  }
  if (gTsens) {
    MmUnmapIoSpace(const_cast<ULONG*>(gTsens), kTsensSize);
    gTsens = nullptr;
  }
  if (gOsm) {
    MmUnmapIoSpace(const_cast<ULONG*>(gOsm), osm::kDomainSize);
    gOsm = nullptr;
  }
}

void Unload(PDRIVER_OBJECT) {
  InterlockedExchange(&gStop, 1);
  KeSetEvent(&gWake, IO_NO_INCREMENT, FALSE);
  if (gThreadObject) {
    KeWaitForSingleObject(gThreadObject, Executive, KernelMode, FALSE, nullptr);
    ObDereferenceObject(gThreadObject);
    gThreadObject = nullptr;
  }
  if (gThread) {
    ZwClose(gThread);
    gThread = nullptr;
  }
  Cleanup();
}

// Retain an address relocation for loading the native image at a kernel VA.
PDRIVER_UNLOAD volatile gUnloadRoutine = Unload;

bool ValidateAndLoadLut() {
  unsigned count = osm::LoadFrequencyTable(OsmRead, gLutKhz);
  if (count == 0) return false;
  gTopLevel = count - 1;
  if (gMaxLevel > gTopLevel) gMaxLevel = gTopLevel;
  return true;
}

}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING regPath) {
  driver->DriverUnload = gUnloadRoutine;
  KeInitializeEvent(&gWake, NotificationEvent, FALSE);
  ReadParameters(regPath);

  gProcessors = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
  if (gProcessors <= kPrimeIndex) return STATUS_NOT_SUPPORTED;

  PHYSICAL_ADDRESS paOsm;
  paOsm.QuadPart = static_cast<LONGLONG>(osm::kDomainBase[2]);
  gOsm = static_cast<volatile ULONG*>(
      MmMapIoSpaceEx(paOsm, osm::kDomainSize, PAGE_READWRITE | PAGE_NOCACHE));
  if (!gOsm) return STATUS_INSUFFICIENT_RESOURCES;

  gOriginal = OsmRead(osm::kRegPerfState);
  if (!ValidateAndLoadLut() || gOriginal > gTopLevel) {
    Cleanup();
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  }

  PHYSICAL_ADDRESS paTsens;
  paTsens.QuadPart = static_cast<LONGLONG>(kTsens0Tm);
  gTsens = static_cast<volatile ULONG*>(
      MmMapIoSpaceEx(paTsens, kTsensSize, PAGE_READONLY | PAGE_NOCACHE));
  if (!gTsens) {
    Cleanup();
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  gTimes = static_cast<CpuTimes*>(
      ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(CpuTimes) * gProcessors, kTag));
  if (!gTimes) {
    Cleanup();
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  NTSTATUS s = PoRegisterPowerSettingCallback(nullptr, &kGuidConsoleDisplayState, DisplayCallback,
                                              nullptr, &gDisplayCallback);
  if (!NT_SUCCESS(s)) {
    Cleanup();
    return s;
  }

  OBJECT_ATTRIBUTES threadOa;
  InitializeObjectAttributes(&threadOa, nullptr, OBJ_KERNEL_HANDLE, nullptr, nullptr);
  s = PsCreateSystemThread(&gThread, THREAD_ALL_ACCESS, &threadOa, nullptr, nullptr, Runner,
                           nullptr);
  if (!NT_SUCCESS(s)) {
    Cleanup();
    return s;
  }
  s = ObReferenceObjectByHandle(gThread, SYNCHRONIZE, *PsThreadType, KernelMode,
                                reinterpret_cast<PVOID*>(&gThreadObject), nullptr);
  if (!NT_SUCCESS(s)) {
    InterlockedExchange(&gStop, 1);
    KeSetEvent(&gWake, IO_NO_INCREMENT, FALSE);
    ZwWaitForSingleObject(gThread, FALSE, nullptr);
    ZwClose(gThread);
    gThread = nullptr;
    Cleanup();
    return s;
  }
  return STATUS_SUCCESS;
}
