# user_init.s -- riscv-dv per-target startup for Ibex Simple System
#
# Included at the very beginning of every generated program, before its own
# initialisation. Simple System needs nothing here: the memory is live out of
# reset and the program sets up its own stack, trap vector and privilege state.
#
# Empty and deliberately so, but the file has to exist for the include.
