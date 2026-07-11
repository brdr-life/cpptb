# Peripheral-suite pure SV vs DPI C++ vs VPI C++ vs cocotb benchmark

- Design: `peripheral_suite: APB timer + APB SPI + APB I2C`
- Iterations per run: `10000`
- Critical tracked DPI/SV pairs: `30`
- Slower VPI/cocotb runs: `3`
- C++ DPI spawn mode: `tracked`
- C++ VPI internal median: `8467.822 ms`
- C++ DPI internal median: `1269.347 ms`
- cocotb internal median: `29326.481 ms`
- C++ VPI/C++ DPI internal ratio: `6.67x`
- cocotb/C++ DPI internal ratio: `23.10x`
- cocotb/C++ VPI internal ratio: `3.46x`
- C++ VPI process median: `8480.382 ms`
- C++ DPI process median: `1274.114 ms`
- Pure SV process median: `1168.829 ms`
- cocotb process median: `29780.141 ms`
- C++ VPI/C++ DPI process ratio: `6.66x`
- cocotb/C++ DPI process ratio: `23.37x`
- cocotb/C++ VPI process ratio: `3.51x`
- C++ DPI/pure SV paired process ratio: `1.086x`
- One-sided 95% upper median bound: `1.096x`
- Two-sided 95% median CI: `[1.078, 1.097]x`
- CI direction: `cpp_dpi_slower`
- C++ VPI/pure SV process ratio: `7.26x`
- cocotb/pure SV process ratio: `25.48x`
- C++ DPI performance guard: `passed` (`1.086x` <= `1.10x`)

DPI and pure SV are warmed, measured as adjacent pairs, and run first
in alternating order. Their reported ratio is the median paired ratio.
The slower VPI and cocotb loops run only after the critical comparison.
