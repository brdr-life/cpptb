// Copyright the cpptb authors.
//
// A bump allocator for libgcc's emulated thread-local storage.
//
// The epmp-tests config compiles `vendor/riscv-isa-sim/tests/mseccfg/syscalls.c`,
// which declares three `__thread` buffers inside its printf path. The toolchain
// this port uses, riscv-none-elf 15.2.0, is built with emulated TLS rather than
// native TLS, so those declarations turn into calls to libgcc's
// `__emutls_get_address`, and that function calls `malloc` and `free`. The
// config also passes `-nostdlib`, so there is no libc to supply them and the
// link fails with:
//
//   libgcc.a(emutls.o): in function `__emutls_get_address':
//   (.text.__emutls_get_address+0x1e): undefined reference to `malloc'
//
// Upstream does not hit this: lowRISC's riscv32-unknown-elf toolchain uses
// native TLS, so emutls.o is never pulled out of the archive.
//
// The allocation is one small block per `__thread` object at first touch and
// nothing is ever freed, so a bump pointer over a fixed pool is enough. The
// pool is sized for the three buffers in syscalls.c with room to spare; running
// out returns null, which `__emutls_get_address` dereferences, so an overflow
// shows up as a fault rather than as silent corruption.

#define EMUTLS_POOL_BYTES 4096

static unsigned char emutls_pool[EMUTLS_POOL_BYTES]
    __attribute__((aligned(16)));
static unsigned long emutls_used;

void *malloc(unsigned long size) {
  unsigned long aligned = (size + 15UL) & ~15UL;
  if (aligned > EMUTLS_POOL_BYTES - emutls_used) {
    return 0;
  }
  void *block = &emutls_pool[emutls_used];
  emutls_used += aligned;
  return block;
}

void free(void *block) { (void)block; }
