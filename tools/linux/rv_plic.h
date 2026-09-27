/* RISC-V 平台级中断控制器实现
 * 参见：https://github.com/riscv/riscv-plic-spec */

#ifndef RV_PLIC_H
#define RV_PLIC_H

#include "rv.h"

#define RV_PLIC_NSRC 32
#define RV_PLIC_NCTX 1

typedef struct rv_plic {
  rv_u32 priority[RV_PLIC_NSRC];
  rv_u32 pending[RV_PLIC_NSRC / 32];
  rv_u32 enable[RV_PLIC_NSRC / 32 * RV_PLIC_NCTX];
  rv_u32 thresh[RV_PLIC_NCTX];
  rv_u32 claim[RV_PLIC_NCTX];
  rv_u32 claiming[RV_PLIC_NSRC / 32]; /* 正在进行 claim 的中断 */
} rv_plic;

/* 初始化 PLIC */
void rv_plic_init(rv_plic *plic);

#define RV_PLIC_SIZE /* 内存映射大小 */ 0x4000000

/* 对 plic 执行一次总线访问 */
rv_res rv_plic_bus(rv_plic *plic, rv_u32 addr, rv_u8 *data, rv_u32 is_store,
                   rv_u32 width);

/* 用给定的中断源请求一次中断 */
rv_res rv_plic_irq(rv_plic *plic, rv_u32 source);

/* 若给定的 hart 需要处理外部中断则返回 1 */
rv_u32 rv_plic_mei(rv_plic *plic, rv_u32 context);

#endif /* RV_PLIC_H */
