// core_ibex's date.c, given C linkage.
//
// ibex_dv.f lists dv/uvm/core_ibex/common/date.c, and Verilator's generated
// makefile compiles every source it is handed with $(CXX). Built as C++ the
// definition is name-mangled, so the DPI import in date_dpi.svh does not
// resolve and the link fails with an undefined reference to
// `get_unix_timestamp'. Including it inside extern "C" restores the linkage a
// C compiler would have given it, without a copy of the function here.
//
// SPDX-License-Identifier: Apache-2.0

extern "C" {
#include "date.c"
}
