/* ============================================================================
 * rv_clint.c —— CLINT（Core-Local Interruptor，核内中断器）
 *
 * 【它负责什么】
 *   CLINT 是每个 hart（硬件线程）一份的"核内"中断源，提供两类中断：
 *     · 软件中断（MSIP）：写 mswi 寄存器即可置起 —— Linux 用它做多核 IPI
 *     · 定时器中断（MTIP）：比较 mtime 与 mtimecmp，达到即置起
 *   它们在 CSR 上分别体现为 mip 的 bit3（MSIP）与 bit7（MTIP），见 M-2.2。
 *
 * 【手册】M-13.2「实现 CLINT 定时器中断」给出了完整的寄存器地址与初始化 /
 *   中断处理代码，可与本文件逐项对照。手册用的是内存映射写法：
 *       CLINT_BASE     = 0x0200_0000
 *       CLINT_MTIMECMP = CLINT_BASE + 0x4000    ← 本文件的 0x4000
 *       CLINT_MTIME    = CLINT_BASE + 0xBFF8    ← 本文件的 0x4000 + 0x7FF8
 *   两处地址都对得上，可直接作为"实现与手册一致"的验证点。
 *   M-13.3 的定时器中断处理代码里，第一步就是"重设 mtimecmp"——这解释了
 *   为什么这个寄存器是 Linux 时间片调度的核心旋钮。
 *
 * 【中断如何真正送达 Linux】还差两段路（见 S-4.3 的委托配置）：
 *   本文件置起 mip.MTIP → OpenSBI（M 模式）把它转为 S 模式的 STIP →
 *   Linux 在 S 模式收到定时器中断。本文件只负责最前面那一段。
 * ============================================================================ */

#include "rv_clint.h"

#include <string.h>

void rv_clint_init(rv_clint *clint, rv *cpu) {
  memset(clint, 0, sizeof(*clint));
  clint->cpu = cpu;
}

/* 【手册】CLINT 的寄存器地址映射 —— 与 M-13.2 里的宏定义逐项对应：
 *     addr = 0x0000               → mswi      （手册称 MSIP；写 1 即产生软件中断）
 *     addr = 0x4000               → mtimecmp  （定时器比较值，低 32 位）
 *     addr = 0x4000 + 4           → mtimecmph （高 32 位）
 *     addr = 0x4000 + 0x7FF8      → mtime     （自由运行计数器，低 32 位）
 *     addr = 0x4000 + 0x7FF8 + 4  → mtimeh    （高 32 位）
 *
 * 一个值得注意的实现细节：mtime 实际存放在 CPU 的 CSR 结构里
 * （cpu->csr.mtime），因为模拟器需要在外层主循环里推进它；而 mtimecmp 是
 * 纯外设状态，就放在 CLINT 自己身上。这也解释了下面 rv_clint_mti() 为什么
 * 能直接拿"CPU 的 mtime"和"CLINT 的 mtimecmp"作比较。
 *
 * 软件侧的标准用法见 M-13.2 的初始化代码：
 *     读 mtime → 加上间隔 → 写回 mtimecmp
 */
rv_res rv_clint_bus(rv_clint *clint, rv_u32 addr, rv_u8 *d, rv_u32 is_store,
                    rv_u32 width) {
  rv_u32 *reg, data;
  rv_endcvt(d, (rv_u8 *)&data, 4, 0);
  if (width != 4)
    return RV_BAD;
  if (addr == 0x0) /*R mswi */
    reg = &clint->mswi;
  else if (addr == 0x4000 + 0x0000) /*R mtimecmp */
    reg = &clint->mtimecmp;
  else if (addr == 0x4000 + 0x0000 + 4) /*R mtimecmph */
    reg = &clint->mtimecmph;
  else if (addr == 0x4000 + 0x7FF8) /*R mtime */
    reg = &clint->cpu->csr.mtime;
  else if (addr == 0x4000 + 0x7FF8 + 4) /*R mtimeh */
    reg = &clint->cpu->csr.mtimeh;
  else
    return RV_BAD;
  if (is_store)
    *reg = data;
  else
    data = *reg;
  rv_endcvt((rv_u8 *)&data, d, 4, 1);
  return RV_OK;
}

rv_u32 rv_clint_msi(rv_clint *clint, rv_u32 context) {
  (void)context; /* 暂时未使用，也许以后再加多核支持 */
  return clint->mswi & 1;
}

rv_u32 rv_clint_mti(rv_clint *clint, rv_u32 context) {
  (void)context;
  return (clint->cpu->csr.mtimeh > clint->mtimecmph) ||
         ((clint->cpu->csr.mtimeh == clint->mtimecmph) &&
          (clint->cpu->csr.mtime >= clint->mtimecmp));
}
