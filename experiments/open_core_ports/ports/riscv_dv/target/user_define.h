# user_define.h -- riscv-dv target macros for Ibex Simple System
#
# riscv-dv opens every generated program with `.include "user_define.h"`, and
# ships an empty stub in vendor/google_riscv-dv/user_extension/. Nothing is
# needed here: Simple System's adaptation is entirely in link.ld, because the
# generated exit sequence
#
#     li gp, 1
#     ecall
#     write_tohost:
#       sw gp, tohost, t5
#
# is HTIF, and Simple System's control register already is the tohost address.
# See target/link.ld.
#
# The file has to exist even so, or the assembler cannot resolve the include.
