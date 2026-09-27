/* RISC-V 核心本地中断器实现
 * 参见：https://github.com/riscv/riscv-aclint */

#ifndef RV_CLINT_H
#define RV_CLINT_H

#include "rv.h"

typedef struct rv_clint {
  rv *cpu;
  rv_u32 mswi, mtimecmp, mtimecmph;
} rv_clint;

/* 为给定的 cpu 初始化中断器 */
void rv_clint_init(rv_clint *clint, rv *cpu);

#define RV_CLINT_SIZE /* 内存映射大小 */ 0x10000

/* 对中断器执行一次总线访问 */
rv_res rv_clint_bus(rv_clint *clint, rv_u32 addr, rv_u8 *data, rv_u32 is_store,
                    rv_u32 width);

/* 若正在发生机器软件中断则返回 1 */
rv_u32 rv_clint_msi(rv_clint *clint, rv_u32 context);

/* 若正在发生机器定时器中断则返回 1 */
rv_u32 rv_clint_mti(rv_clint *clint, rv_u32 context);

#endif /* RV_CLINT_H */
