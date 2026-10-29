# NabuPrimeCore

A frequency governor for the prime core of the Xiaomi Pad 5 (nabu, Snapdragon 860) running Windows on
ARM.

This fork discovers the hardware table at driver load instead of requiring the upstream 21-level
table. The load governor, temperature thresholds, display handling and unload restoration are
retained. Upstream performance measurements below have not been remeasured for this fork.

The SD860 has one prime core (CPU7, up to 2.96 GHz). Windows' power driver on this platform only
manages the silver and gold clusters, so CPU7 stays at its lowest frequency, 825.6 MHz, for the
whole session, even at 100 % load. NabuPrime gives CPU7 its frequency scaling back:

- CPU7 reaches the highest frequency in its firmware table within about 0.1 s of load and drops back
  within about 0.2 s when idle (up to 2956.8 MHz; 2841.6 MHz on the tested 20-level table).
- Single-thread work pinned to CPU7 runs about 3.5× faster than stock.
- CPU7 is capped by temperature, from its own two sensors.
- CPU7 is parked at its lowest level while the screen is off.

## How it works

NabuPrime is a small kernel driver that controls only CPU7's frequency domain in the SoC's CPU clock
hardware. At every driver load it reads CPU7's actual hardware frequency table, ending at the first
repeated frequency. Every 50 ms it reads CPU7's load and picks a level from that detected table.
Above 85 % load it goes straight to the allowed top; below that, it targets
1.25 × the load, the same rule as Linux's schedutil governor.

The thermal limit is a fixed limit per temperature, using the hotter of CPU7's two sensors averaged
over about 2 s:

| CPU7 temperature | up to 86.5 °C | 88.5 °C | 90 °C | 92.5 °C | 94 °C | 95 °C |
|---|---|---|---|---|---|---|
| highest CPU7 frequency | 2956.8 MHz | 2534.4 MHz | 2227.2 MHz | 1708.8 MHz | 1171.2 MHz | 825.6 MHz |

These are the original frequency ceilings. Each ceiling is mapped down to an available hardware
level, so a shorter or differently spaced table does not raise the original thermal frequency limits.
The sensor averaging, hysteresis and recovery timing are unchanged.

Nothing else on this platform slows CPU7 down when it gets hot, so this limit is CPU7's only thermal
protection. Under a sustained all-core plus GPU load the die settles around 88–92 °C and CPU7 averages
about 2 GHz.

The driver accepts strictly increasing hardware frequencies starting at 825.6 MHz and no higher
than the SD860's 2956.8 MHz limit. It requires a duplicate terminator within the 40-row hardware LUT
and a valid original performance level. Disabled, descending, out-of-range or unterminated tables
still prevent loading. It reads the firmware's table; it does not rewrite frequencies or voltages.
Stopping it puts CPU7 back to the level it had before it started.

## Install

Requirements: Windows on ARM on the Xiaomi Pad 5 with the Windows On Andromeda platform drivers. The
driver is test-signed, so test signing must be on (and Secure Boot off).

1. Turn on test signing in an administrator terminal, then restart:

   ```
   bcdedit /set testsigning on
   ```

2. Download `nabu-prime-<version>.zip` from [Releases](../../releases) and extract it.
3. Open PowerShell as administrator in the extracted folder and run:

   ```
   Set-ExecutionPolicy -Scope Process Bypass
   .\install.ps1
   ```

   The script trusts the included test certificate, installs the driver and starts it. Fork releases
   use a separate CLF184 test certificate, not the upstream Nabu USB Drivers certificate. It starts
   automatically at every boot after that. No restart is needed.

## Uninstall

In the same folder, as administrator: `.\uninstall.ps1`. CPU7 returns to stock at once; no restart
is needed.

## Settings

Under `HKLM\SYSTEM\CurrentControlSet\Services\NabuPrime\Parameters` (DWORD values, applied when the
driver starts):

- `MaxLevel`: the highest hardware-table index. Default `0xffffffff` selects the detected maximum;
  larger values are clamped to the last valid level. For example, 19 is the maximum on a 20-level
  table and 20 on a 21-level table. Existing explicit index limits remain index limits.
- `LogIntervalMs`: 0 = no log (default). Otherwise the driver writes `C:\nabu\osm\prime.log` at that
  interval; the folder must exist. `.\install.ps1 -LogSeconds 60` sets this up.

## Source

`drivers/nabu-prime/` holds the driver source (C++17, WDM, ARM64) and its INF. Signed packages are
published under Releases.

On Windows, `tools/build.ps1` builds an unsigned ARM64 driver with Zig 0.14.1, Python 3, WDK ARM64
and Windows SDK headers/libraries (tested with WDK 10.0.26100.6584 and SDK 10.0.26100.1):

```powershell
.\tools\build.ps1 -Zig C:\tools\zig\zig.exe -WdkRoot C:\kits\wdk\c -SdkRoot C:\kits\sdk\c -RunTests
```

`WdkRoot` and `SdkRoot` must contain the kit's `Include` directory; `WdkRoot` also contains
`Lib\10.0.26100.0\km\ARM64`. The default output is `build/NabuPrime.sys`. Signing and package
catalog generation use the Windows SDK tools and a separate test certificate; private keys are
not part of this repository.

`tools/package.ps1` takes `-BuildDir`, `-Inf2Cat`, `-SignTool` and a separate, passwordless test
`-CertificatePfx` to embed-sign the driver, generate/sign its ARM64 catalog, and package the original
installation workflow from `package/`. Keep the signing key outside published artifacts.

## Tests

The portable table tests exercise the same loader and frequency-ceiling mapping used by the driver:

```
c++ -std=c++17 tests/frequency_table_test.cpp -o frequency-table-test
./frequency-table-test tests/fixtures/nabu-2841-lut.txt
```

The fixture contains only the 40 raw CPU7 LUT rows from a Pad 5 running the 2608.03 nSB firmware and
qcpep 1.0.2140.0. It reproduces the original driver's rejection of a table ending at 2841.6 MHz.
Other cases cover the original 21-level table, a non-reference intermediate frequency, malformed
tables, maximum table size and preserving the original thermal frequency ceilings.

## Thanks

- Windows On Andromeda (Gustave Monce and contributors): the platform drivers and UEFI that Windows on
  the Xiaomi Pad 5 runs on.
- The Linux kernel's Qualcomm CPU frequency driver (`qcom-cpufreq-hw.c`) and the mainline SM8150
  device tree: the reference for the CPU clock hardware's register layout, the frequency domain
  addresses and CPU7's temperature sensors. No code was copied.
- Linux's schedutil governor: the frequency target rule.

## Disclaimer

This driver is provided as is, without any warranty. It runs CPU7 up to its rated maximum
frequency, which makes the tablet warmer under load. Use it at your own risk: the authors take no
responsibility for any damage to your device, battery or data.

## License

GPL-2.0, see [LICENSE](LICENSE).
