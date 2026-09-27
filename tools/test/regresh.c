#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../rv.h"

#define RAM_BASE 0x80000000
#define RAM_SIZE 0x10000

rv_res
bus_cb(void* user, rv_u32 addr, rv_u8* data, rv_u32 is_store, rv_u32 width)
{
  rv_u8* mem = (rv_u8*)user + addr - RAM_BASE;
  if (addr < RAM_BASE || addr + width >= RAM_BASE + RAM_SIZE)
    return RV_BAD;
  memcpy(is_store ? mem : data, is_store ? data : mem, width);
  return RV_OK;
}

/* https://github.com/mnurzia/rv/issues/5 */
int test_5(void)
{
  /* 确保除以零不会让宿主机陷入 trap，即使编码是非法的 */
  rv cpu;
  rv_u8 mem[RAM_SIZE];
  rv_u32 prog[5] = {
      0x00500193, /* addi x3, x0, 5 */
      0x0201f0b3, /* remu x1, x3, x1 */
      0x0201e0b3, /* rem x1, x3, x1 */
      0x42437733, /* 非法指令 */
      0x3384F333, /* 非法指令 */
  };
  memcpy((void*)mem, prog, sizeof(prog));
  rv_init(&cpu, mem, &bus_cb);
  assert(rv_step(&cpu) == RV_TRAP_NONE);
  assert(rv_step(&cpu) == RV_TRAP_NONE);
  assert(cpu.r[1] == 5);
  assert(rv_step(&cpu) == RV_TRAP_NONE);
  assert(cpu.r[1] == 5);
  assert(rv_step(&cpu) == RV_EILL); /* 确保第一条非法指令会 trap */
  cpu.pc = 0x80000010; /* 执行最后一条指令 */
  assert(rv_step(&cpu) == RV_EILL); /* 确保它也会 trap */
  return 0;
}

int main(void) { assert(!test_5()); }
