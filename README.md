# NabuPrimeCore

A frequency governor for the prime core of the Xiaomi Pad 5 (nabu, Snapdragon 860) running Windows on
ARM.

The SD860 has one prime core (CPU7, up to 2.96 GHz). Windows' power driver on this platform only
manages the silver and gold clusters, so CPU7 stays at its lowest frequency, 825.6 MHz, for the
whole session, even at 100 % load. NabuPrime gives CPU7 its frequency scaling back:

- CPU7 reaches 2956.8 MHz within about 0.1 s of load and drops back within about 0.2 s when idle.
- Single-thread work pinned to CPU7 runs about 3.5× faster than stock.
- CPU7 is capped by temperature, from its own two sensors.
- CPU7 is parked at its lowest level while the screen is off.

## How it works

NabuPrime is a small kernel driver that controls only CPU7's frequency domain in the SoC's CPU clock
hardware. Every 50 ms it reads CPU7's load and picks a level from CPU7's own 21-level frequency table
(825.6 to 2956.8 MHz). Above 85 % load it goes straight to the allowed top; below that, it targets
1.25 × the load, the same rule as Linux's schedutil governor.

The thermal limit is a fixed limit per temperature, using the hotter of CPU7's two sensors averaged
over about 2 s:

| CPU7 temperature | up to 86.5 °C | 88.5 °C | 90 °C | 92.5 °C | 94 °C | 95 °C |
|---|---|---|---|---|---|---|
| highest CPU7 frequency | 2956.8 MHz | 2534.4 MHz | 2227.2 MHz | 1708.8 MHz | 1171.2 MHz | 825.6 MHz |

Nothing else on this platform slows CPU7 down when it gets hot, so this limit is CPU7's only thermal
protection. Under a sustained all-core plus GPU load the die settles around 88–92 °C and CPU7 averages
about 2 GHz.

The driver refuses to load unless CPU7's frequency table is exactly the SD860 table it was made for.
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

   The script trusts the "Nabu USB Drivers" certificate (the same one UsbNabu uses), installs the
   driver and starts it. It starts automatically at every boot after that. No restart is needed.

## Uninstall

In the same folder, as administrator: `.\uninstall.ps1`. CPU7 returns to stock at once; no restart
is needed.

## Settings

Under `HKLM\SYSTEM\CurrentControlSet\Services\NabuPrime\Parameters` (DWORD values, applied when the
driver starts):

- `MaxLevel`: the highest CPU7 level, 0–20 (default 20 = 2956.8 MHz).
- `LogIntervalMs`: 0 = no log (default). Otherwise the driver writes `C:\nabu\osm\prime.log` at that
  interval; the folder must exist. `.\install.ps1 -LogSeconds 60` sets this up.

## Source

`drivers/nabu-prime/` holds the driver source (C++17, WDM, ARM64) and its INF. Signed packages are
published under Releases.

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
