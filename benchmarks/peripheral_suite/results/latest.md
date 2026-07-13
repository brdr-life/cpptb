# Peripheral-suite pure SV vs DPI C++ vs VPI C++ vs cocotb benchmark

- Design: `peripheral_suite: APB timer + APB SPI + APB I2C`
- Iterations per run: `10000`
- Critical tracked DPI/SV pairs: `16`
- Slower VPI/cocotb runs: `3`
- C++ DPI spawn mode: `tracked`
- C++ VPI internal median: `7450.046 ms`
- C++ DPI internal median: `933.206 ms`
- cocotb internal median: `23040.405 ms`
- C++ VPI/C++ DPI internal ratio: `7.98x`
- cocotb/C++ DPI internal ratio: `24.69x`
- cocotb/C++ VPI internal ratio: `3.09x`
- C++ VPI process median: `7458.852 ms`
- C++ DPI process median: `937.347 ms`
- Pure SV process median: `954.944 ms`
- cocotb process median: `23351.429 ms`
- C++ VPI/C++ DPI process ratio: `7.96x`
- cocotb/C++ DPI process ratio: `24.91x`
- cocotb/C++ VPI process ratio: `3.13x`
- C++ DPI/pure SV paired process ratio: `0.986x`
- DPI-first paired median: `0.992x`
- SV-first paired median: `0.980x`
- Independent-median ratio: `0.982x`
- Independent/paired relative disagreement: `0.44%`
- Order-stratum gap: `1.21%`
- One-sided 95% upper median bound: `0.994x`
- Two-sided 95% median CI: `[0.964, 0.996]x`
- CI direction: `cpp_dpi_faster`
- C++ VPI/pure SV process ratio: `7.81x`
- cocotb/pure SV process ratio: `24.45x`
- C++ DPI performance guard: `passed` (`0.986x` <= `1.10x`)

DPI and pure SV are warmed, measured as adjacent pairs, and run first
in alternating order. Their reported ratio is the median paired ratio.
The slower VPI and cocotb loops run only after the critical comparison.
