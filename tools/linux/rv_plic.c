/* ============================================================================
 * rv_plic.c —— PLIC（Platform-Level Interrupt Controller，平台级中断控制器）
 *
 * 【它负责什么】
 *   把所有外设的中断汇聚起来，按优先级挑一个报告给 hart。它属于"平台定义"
 *   （PLIC 规范），不在 RISC-V 特权手册范围内；但它产生的外部中断最终体现为
 *   CSR 的 mip.MEIP(bit11) / mip.SEIP(bit9)（见 M-2.2），并且要配合 S-4.3 的
 *   委托配置才能送达 Linux —— 本仓库走的是 SEIP 路线，理由见 rv.c 里
 *   rv_irq 的注释。
 *
 * 【两阶段握手：claim + complete】
 *   PLIC 的标准用法不是"读取中断号"这么简单，而是：
 *     ① 读 claim 寄存器 → 硬件返回当前最高优先级的中断号，并把它标记为"处理中"；
 *     ② 软件处理完 → 把这个中断号【写回】同一个 claim 寄存器，即 complete。
 *   本文件用 claiming[] 位图记录"已 claim、尚未 complete"的中断，
 *   既是符合规范的建模，也能暴露驱动程序忘记 complete 时的行为。
 * ============================================================================ */

#include "rv_plic.h"

#include <stdio.h>
#include <string.h>

void rv_plic_init(rv_plic *plic) { memset(plic, 0, sizeof(*plic)); }

rv_res rv_plic_bus(rv_plic *plic, rv_u32 addr, rv_u8 *d, rv_u32 is_store,
                   rv_u32 width) {
  rv_u32 *reg = NULL, wmask = 0 - 1U, data;
  rv_endcvt(d, (rv_u8 *)&data, 4, 0);
  if (addr >= RV_PLIC_SIZE || width != 4)
    return RV_BAD;
  else if (addr < RV_PLIC_NSRC * 4) /*R 中断源优先级 */
    reg = plic->priority + (addr >> 2), wmask *= !!addr;
  else if (addr >= 0x1000 &&
           addr < 0x1000 + RV_PLIC_NSRC / 8) /*R 中断挂起位 */
    reg = plic->pending + ((addr - 0x1000) >> 2), wmask ^= addr == 0x1000;
  else if (addr >= 0x2000 &&
           addr < 0x2000 + RV_PLIC_NSRC / 8) /*R 中断使能位 */
    reg = plic->enable + ((addr - 0x2000) >> 2), wmask ^= addr == 0x2000;
  else if (addr >> 12 >= 0x200 && (addr >> 12) < 0x200 + RV_PLIC_NCTX &&
           !(addr & 0xFFF)) /*R 优先级阈值 */
    reg = plic->thresh + ((addr >> 12) - 0x200);
  else if (addr >> 12 >= 0x200 && (addr >> 12) < 0x200 + RV_PLIC_NCTX &&
           (addr & 0xFFF) == 4) /*R 中断 claim 寄存器 */ {
    rv_u32 context = (addr >> 12) - 0x200, en_off = context * RV_PLIC_NSRC / 32;
    reg = plic->claim + context;
    if (!is_store && *reg < RV_PLIC_NSRC) {
      if (plic->pending[*reg / 32] & (1U << *reg % 32))
        plic->claiming[*reg / 32 + en_off] |=
            1U << *reg % 32; /* 置位 claiming 位 */
    } else if (is_store && data < RV_PLIC_NSRC) {
      plic->claiming[data / 32 + en_off] &=
          ~(1U << data % 32); /* 清除 claiming 位 */
    }
  }
  if (reg && !is_store)
    data = *reg;
  else if (reg)
    *reg = (*reg & ~wmask) | (data & wmask);
  rv_endcvt((rv_u8 *)&data, d, 4, 0);
  return RV_OK;
}

rv_res rv_plic_irq(rv_plic *plic, rv_u32 source) {
  if (source > RV_PLIC_NSRC || !source ||
      ((plic->claiming[source / 32] >> (source % 32)) & 1U) ||
      ((plic->pending[source / 32] >> (source % 32)) & 1U))
    return RV_BAD;
  plic->pending[source / 32] |= 1U << source % 32;
  return RV_OK;
}

rv_u32 rv_plic_mei(rv_plic *plic, rv_u32 context) {
  rv_u32 i, j, o = 0, h = 0;
  for (i = 0; i < RV_PLIC_NSRC / 32; i++) {
    rv_u32 en_off = i + context * RV_PLIC_NSRC / 32;
    if (!((plic->enable[en_off] & plic->pending[i]) | plic->claiming[i]))
      continue;
    for (j = 0; j < 32; j++) {
      if ((plic->claiming[en_off] >> j) & 1U)
        plic->pending[i] &= ~(1U << j);
      else if (((plic->enable[i] >> j) & 1U) &&
               ((plic->pending[i] >> j) & 1U) &&
               plic->priority[i * 32 + j] >= h &&
               plic->priority[i * 32 + j] >= plic->thresh[context])
        o = i * 32 + j, h = plic->priority[i * 32 + j];
    }
  }
  plic->claim[context] = o;
  return !!o;
}
