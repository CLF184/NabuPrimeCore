NabuPrime 1.0.3.0 dynamic-LUT fork (CLF184/NabuPrimeCore)

Reads CPU7's hardware frequency table at every driver load, discovers its valid
levels, and uses those actual frequencies for the original load governor.
Supports the observed 20-level table (825.6-2841.6 MHz) and the upstream 21-level
table (825.6-2956.8 MHz), without hardcoding either as the required OPP list.
The original thermal frequency ceilings, sensors, timing, display-off parking
and original-level restoration are preserved. Unsupported/malformed tables
still prevent loading; no frequencies or voltages are rewritten.

This is a test-signed prerelease. The driver was compiled and regression tested,
but the modified governor has not yet been exercised on the physical tablet.
The included certificate is this fork's own test certificate, not the upstream
Nabu USB Drivers certificate. No private signing key is included.

Needs test signing enabled and Secure Boot off, as with the upstream driver.
Install from administrator PowerShell in this folder:
    Set-ExecutionPolicy -Scope Process Bypass
    .\install.ps1 -LogSeconds 1

This also sets up C:\nabu\osm\prime.log; its header shows the detected LUT size
and minimum/maximum frequency. Reinstall without -LogSeconds to disable logging.
Uninstall: .\uninstall.ps1

MaxLevel defaults to 0xffffffff (the detected maximum). Explicit numeric index
limits are clamped to the detected highest valid level.

Source and license: https://github.com/CLF184/NabuPrimeCore (GPL-2.0).
