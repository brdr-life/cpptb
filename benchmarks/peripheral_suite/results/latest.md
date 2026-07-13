# Peripheral-suite pure SV vs DPI C++ vs VPI C++ vs cocotb benchmark

- Design: `peripheral_suite: APB timer + APB SPI + APB I2C`
- Iterations per run: `10000`
- Critical tracked DPI/SV pairs: `16`
- Slower VPI/cocotb runs: `3`
- C++ DPI spawn mode: `tracked`
- C++ VPI internal median: `7457.463 ms`
- C++ DPI internal median: `939.036 ms`
- cocotb internal median: `22831.309 ms`
- C++ VPI/C++ DPI internal ratio: `7.94x`
- cocotb/C++ DPI internal ratio: `24.31x`
- cocotb/C++ VPI internal ratio: `3.06x`
- C++ VPI process median: `7466.835 ms`
- C++ DPI process median: `943.098 ms`
- Pure SV process median: `965.487 ms`
- cocotb process median: `23129.906 ms`
- C++ VPI/C++ DPI process ratio: `7.92x`
- cocotb/C++ DPI process ratio: `24.53x`
- cocotb/C++ VPI process ratio: `3.10x`
- C++ DPI/pure SV paired process ratio: `0.975x`
- DPI-first paired median: `0.977x`
- SV-first paired median: `0.971x`
- Independent-median ratio: `0.977x`
- Independent/paired relative disagreement: `0.21%`
- Order-stratum gap: `0.63%`
- One-sided 95% upper median bound: `0.983x`
- Two-sided 95% median CI: `[0.965, 0.986]x`
- CI direction: `cpp_dpi_faster`
- C++ VPI/pure SV process ratio: `7.73x`
- cocotb/pure SV process ratio: `23.96x`
- C++ DPI performance guard: `passed` (`0.975x` <= `1.10x`)

DPI and pure SV are warmed, measured as adjacent pairs, and run first
in alternating order. Their reported ratio is the median paired ratio.
The slower VPI and cocotb loops run only after the critical comparison.
