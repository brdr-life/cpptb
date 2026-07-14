# Third-party RTL

These sources are vendored verbatim so benchmark builds are reproducible and
do not require network access.

| Directory | Upstream | Pinned commit | License | Files used |
|---|---|---|---|---|
| `picorv32` | [YosysHQ/picorv32](https://github.com/YosysHQ/picorv32) | `87c89acc18994c8cf9a2311e871818e87d304568` | ISC | `picorv32.v` |
| `secworks_aes` | [secworks/aes](https://github.com/secworks/aes) | `80dc4718e1dcbbdb4b0dd1bdb393d8f7b98981dc` | BSD-2-Clause | AES top and core RTL |
| `verilog_ethernet` | [alexforencich/verilog-ethernet](https://github.com/alexforencich/verilog-ethernet) | `77320a9471d19c7dd383914bc049e02d9f4f1ffb` | MIT | `axis_eth_fcs.v`, `lfsr.v` |

Each directory includes the corresponding upstream license. cpptb-specific
wrappers and testbenches live outside `third_party/`.
