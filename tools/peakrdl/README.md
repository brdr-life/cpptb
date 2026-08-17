# cpptb-peakrdl

The SystemRDL / IP-XACT register-model exporter for
[cpptb](https://cpptb-docs.pages.dev), packaged as a
[PeakRDL](https://peakrdl.readthedocs.io) plugin. It generates the typed C++
register model (`RegModel`) that cpptb testbenches drive.

## Install

```sh
pip install cpptb-peakrdl[peakrdl,ipxact]
```

The `peakrdl` extra installs the PeakRDL command line this plugin registers
with; `ipxact` adds IP-XACT input support. Both are optional so that the
plugin itself stays dependency-free: cpptb's own `cpptb-rggen` path uses the
same emitter without any of them.

## Use

```sh
peakrdl cpptb registers.rdl -o build/generated/registers.hpp --namespace my_regs
```

The complete generator reference, naming controls, and limitations are
documented at
[cpptb's register-generation guide](https://cpptb-docs.pages.dev/verification-components/register-generation).

## License

Apache-2.0. Note that the PeakRDL packages themselves are LGPLv3
(`peakrdl-ipxact`: GPLv3); they are deliberately extras rather than
dependencies, and cpptb never bundles them.
