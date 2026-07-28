// UVM's DPI for Verilator.
//
// uvm_dpi.cc unconditionally includes uvm_hdl.c, which selects a vendor backend
// with `#if defined(VCS) || defined(QUESTA) || defined(XCELIUM)` and otherwise
// stops at `#error "hdl vendor backend is missing"`. Verilator is none of
// those, so this compiles the vendor-neutral parts of UVM's DPI directly and
// supplies the HDL backend below.
//
// The other three parts need no adaptation:
//   uvm_common.c     reporting back into UVM
//   uvm_regex.cc     POSIX regex, used by the config DB and factory
//   uvm_svcmd_dpi.c  the plusarg list, over vpi_get_vlog_info
//
// uvm_svcmd_dpi.c is why the build passes --vpi even before any HDL access:
// without it uvm_cmdline_processor sees no arguments and +UVM_TESTNAME never
// reaches run_test().
//
// This file is a copy of ports/core_ibex_uvm/shims/uvm_dpi_verilator.cc,
// unchanged. The two ports build different testbenches and are kept
// independent of each other; UVM's DPI is the same either way.
//
// SPDX-License-Identifier: Apache-2.0

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <string.h>

#include "uvm_dpi.h"

// Declared before the includes to avoid -Wmissing-declarations, matching what
// uvm_dpi.cc does for the same reason.
int uvm_hdl_check_path(char *path);
int uvm_hdl_read(char *path, p_vpi_vecval value);
int uvm_hdl_deposit(char *path, p_vpi_vecval value);
int uvm_hdl_force(char *path, p_vpi_vecval value);
int uvm_hdl_release_and_read(char *path, p_vpi_vecval value);
int uvm_hdl_release(char *path);
const char *uvm_dpi_get_next_arg_c(int init);
extern char *uvm_dpi_get_tool_name_c();
extern char *uvm_dpi_get_tool_version_c();
extern char *uvm_re_buffer();
extern const char *uvm_re_deglobbed(const char *glob, unsigned char with_brackets);
extern void uvm_re_free(regex_t *handle);
extern regex_t *uvm_re_comp(const char *re, unsigned char deglob);
extern int uvm_re_exec(regex_t *rexp, const char *str);
extern regex_t *uvm_re_compexec(const char *re, const char *str,
                                unsigned char deglob, int *exec_ret);

#include "uvm_common.c"
#include "uvm_regex.cc"
#include "uvm_svcmd_dpi.c"

// ---------------------------------------------------------------------------
// The HDL backend, over plain VPI.
//
// Verilator only exposes a signal to VPI when it is marked public, and only
// allows a write when it is public_flat_rw. Nothing is public by default, so a
// path the testbench asks for has to be named in verilator.vlt. A missing name
// surfaces here as a null handle, which is what uvm_hdl_check_path reports and
// what the `DV_CHECK_FATAL wrapped around it in core_ibex_test_lib.sv turns
// into a fatal rather than a silent zero.

static vpiHandle uvm_hdl_lookup(const char *path, const char *context) {
    vpiHandle handle = vpi_handle_by_name((PLI_BYTE8 *)path, NULL);
    if (handle == NULL) {
        char message[1024];
        snprintf(message, sizeof(message),
                 "unable to locate hdl path (%s); with Verilator the signal "
                 "must be named in verilator.vlt as public_flat_rw",
                 path);
        m_uvm_report_dpi(M_UVM_ERROR, (char *)context, message, M_UVM_NONE,
                         (char *)__FILE__, __LINE__);
    }
    return handle;
}

static int uvm_hdl_transfer(char *path, p_vpi_vecval value, PLI_INT32 flags,
                            const char *context) {
    vpiHandle handle = uvm_hdl_lookup(path, context);
    if (handle == NULL) return 0;

    s_vpi_value value_s;
    s_vpi_time time_s = {vpiSimTime, 0, 0, 0.0};
    value_s.format = vpiVectorVal;
    value_s.value.vector = value;
    vpi_put_value(handle, &value_s, &time_s, flags);

    const int ok = (vpi_chk_error(NULL) == 0);
    if (!ok) {
        char message[1024];
        snprintf(message, sizeof(message), "unable to write hdl path (%s)", path);
        m_uvm_report_dpi(M_UVM_ERROR, (char *)context, message, M_UVM_NONE,
                         (char *)__FILE__, __LINE__);
    }
    vpi_release_handle(handle);
    return ok;
}

int uvm_hdl_check_path(char *path) {
    vpiHandle handle = vpi_handle_by_name((PLI_BYTE8 *)path, NULL);
    if (handle == NULL) return 0;
    vpi_release_handle(handle);
    return 1;
}

int uvm_hdl_read(char *path, p_vpi_vecval value) {
    vpiHandle handle = uvm_hdl_lookup(path, "UVM/DPI/HDL_GET");
    if (handle == NULL) return 0;

    s_vpi_value value_s;
    value_s.format = vpiVectorVal;
    vpi_get_value(handle, &value_s);
    if (vpi_chk_error(NULL) != 0) {
        vpi_release_handle(handle);
        return 0;
    }

    // vpi_get_value returns a buffer VPI owns and may reuse, so the words are
    // copied out rather than the pointer handed back. The size is in bits and
    // packs into 32-bit words, matching p_vpi_vecval.
    const int bits = vpi_get(vpiSize, handle);
    const int words = (bits <= 0) ? 1 : ((bits - 1) / 32 + 1);
    for (int i = 0; i < words; ++i) {
        value[i].aval = value_s.value.vector[i].aval;
        value[i].bval = value_s.value.vector[i].bval;
    }
    vpi_release_handle(handle);
    return 1;
}

int uvm_hdl_deposit(char *path, p_vpi_vecval value) {
    return uvm_hdl_transfer(path, value, vpiNoDelay, "UVM/DPI/HDL_DEPOSIT");
}

int uvm_hdl_force(char *path, p_vpi_vecval value) {
    return uvm_hdl_transfer(path, value, vpiForceFlag, "UVM/DPI/HDL_FORCE");
}

int uvm_hdl_release(char *path) {
    // The value is ignored under vpiReleaseFlag but a buffer still has to be
    // supplied. UVM_HDL_MAX_WIDTH is 1024 bits unless overridden.
    s_vpi_vecval scratch[1024 / 32] = {};
    return uvm_hdl_transfer(path, scratch, vpiReleaseFlag, "UVM/DPI/HDL_RELEASE");
}

int uvm_hdl_release_and_read(char *path, p_vpi_vecval value) {
    if (!uvm_hdl_release(path)) return 0;
    return uvm_hdl_read(path, value);
}

#ifdef __cplusplus
}
#endif
