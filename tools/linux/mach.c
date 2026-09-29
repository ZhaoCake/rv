/* ============================================================================
 * mach.c —— 一台"能跑 Linux 的机器"（主板模型）
 *
 * 【这个文件在做什么】
 *   rv.c 只提供 CPU 核；本文件补上它周围的一切：内存、地址解码、三个外设
 *   （CLINT / PLIC / UART），以及"开机第一节拍"要做的事。
 *   两者合起来，才是一台能被 OpenSBI + Linux 使用的机器。
 *
 * 【手册】涉及特权架构 / 平台规范之处，注释用 `【手册】` 标注出处：
 *       M-x.y  →  docs/notes/linux/RISC-V-M模式特权架构详解.pdf   第 x.y 节
 *       S-x.y  →  docs/notes/linux/RISC-V-S模式特权架构详解.pdf   第 x.y 节
 *   最值得配合读的三处：
 *       CLINT 定时器   ←→  M-13.2「实现 CLINT 定时器中断」
 *       中断注入       ←→  M-2.2「mie / mip」（配合 rv.c 的 rv_irq）
 *       启动约定 a0/a1 ←→  S-11.2「OpenSBI 的角色」、S-11.3「M → S 的跳转代码」
 *
 * 【重要】描述同一台硬件有"两份且必须一致"的东西：
 *   本文件的地址映射常量   ↔   tools/linux/extern/rv.dts 里各节点的 reg
 *   两者一旦不一致，Linux 就找不到设备（详见 docs/TASK.md 的 todoK / todoL）。
 * ============================================================================ */

#include <curses.h>
#include <stdlib.h>
#include <string.h>

#include "rv.h"
#include "rv_clint.h"
#include "rv_plic.h"
#include "rv_uart.h"

/* ── 地址映射 ─────────────────────────────────────────────────────────────
 * 【必须与设备树一致】下表是"机器侧"的说法；Linux 侧的说法在
 * tools/linux/extern/rv.dts 里。两者对不上，Linux 就枚举不到设备。
 *
 *   本文件常量          地址 / 大小               rv.dts 中对应节点
 *   MACH_RAM_BASE       0x8000_0000 / 128MiB     memory@80000000
 *   MACH_CLINT0_BASE    0x0200_0000 / 0x10000    clint@2000000
 *   MACH_PLIC0_BASE     0x0C00_0000 / 64MiB      interrupt-controller@c000000
 *   MACH_UART0_BASE     0x0300_0000 / 0x20       serial@3000000（中断号 1）
 *   MACH_UART1_BASE     0x0600_0000 / 0x20       serial@6000000（中断号 2）
 *
 * 【容易看错的一处】DTB 的物理地址是 MACH_RAM_BASE + MACH_DTB_OFFSET
 *   = 0x8000_0000 + 0x0200_0000 = 【0x8200_0000】（在 RAM 内偏移 32MiB），
 *   不要与 CLINT 的 【0x0200_0000】 混淆 —— 数字相似，但一个是内存、一个是外设。
 *
 * 【手册】CLINT 的寄存器布局（mswi / mtimecmp / mtime）见 M-13.2
 *   「实现 CLINT 定时器中断」；PLIC 与 UART 属于平台定义，不在特权手册范围内。
 * ──────────────────────────────────────────────────────────────────────── */
#define MACH_RAM_BASE 0x80000000UL
#define MACH_RAM_SIZE (1024UL * 1024UL * 128UL) /* 128MiB 内存 */
#define MACH_DTB_OFFSET 0x2000000UL             /* dtb 位于 RAM 内 32MiB 处 */

#define MACH_PLIC0_BASE 0xC000000UL  /* plic0 基地址 */
#define MACH_CLINT0_BASE 0x2000000UL /* clint0 基地址 */
#define MACH_UART0_BASE 0x3000000UL  /* uart0 基地址 */
#define MACH_UART1_BASE 0x6000000UL  /* uart1 基地址 */

typedef struct mach {
  rv *cpu;
  rv_u8 *ram;
  rv_plic plic0;
  rv_clint clint0;
  rv_uart uart0, uart1;
} mach;

/* 机器通用总线访问 */
rv_res mach_bus(void *user, rv_u32 addr, rv_u8 *data, rv_u32 store,
                rv_u32 width) {
  mach *m = (mach *)user;
  if (addr >= MACH_RAM_BASE && addr < MACH_RAM_BASE + MACH_RAM_SIZE) {
    rv_u8 *ram = m->ram + addr - MACH_RAM_BASE;
    memcpy(store ? ram : data, store ? data : ram, width);
    return RV_OK;
  } else if (addr >= MACH_PLIC0_BASE && addr < MACH_PLIC0_BASE + RV_PLIC_SIZE) {
    return rv_plic_bus(&m->plic0, addr - MACH_PLIC0_BASE, data, store, width);
  } else if (addr >= MACH_CLINT0_BASE &&
             addr < MACH_CLINT0_BASE + RV_CLINT_SIZE) {
    return rv_clint_bus(&m->clint0, addr - MACH_CLINT0_BASE, data, store,
                        width);
  } else if (addr >= MACH_UART0_BASE && addr < MACH_UART0_BASE + RV_UART_SIZE) {
    return rv_uart_bus(&m->uart0, addr - MACH_UART0_BASE, data, store, width);
  } else if (addr >= MACH_UART1_BASE && addr < MACH_UART1_BASE + RV_UART_SIZE) {
    return rv_uart_bus(&m->uart1, addr - MACH_UART1_BASE, data, store, width);
  } else {
    return RV_BAD;
  }
}

/* uart0 I/O 回调 */
rv_res uart0_io(void *user, rv_u8 *byte, rv_u32 write) {
  int ch;
  static int thrott = 0; /* 防止 getch() 被调用得过于频繁 */
  (void)user;
  if (write && *byte != '\r') /* 回显 '\r' 会让 curses 出错 */
    echochar(*byte);
  else if (!write && ((thrott = (thrott + 1) & 0xFFF) || (ch = getch()) == ERR))
    return RV_BAD;
  else if (!write)
    *byte = (rv_u8)ch;
  return RV_OK;
}

/* uart1 I/O 回调 */
rv_res uart1_io(void *user, rv_u8 *byte, rv_u32 write) {
  (void)user, (void)byte, (void)write;
  /* 你自己的 uart，想怎么用就怎么用！ */
  return RV_BAD; /* 暂时留空 */
}

/* 简易 bootrom */
void load(const char *path, rv_u8 *buf, rv_u32 max_size) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    printf("unable to load file %s\n", path);
    exit(EXIT_FAILURE);
  }
  fread(buf, 1, max_size, f);
  fclose(f);
}

int main(int argc, const char *const *argv) {
  rv cpu;
  mach m;
  rv_u32 rtc_period = 0;
  size_t ninst = 0, ctr = 0;

  if (argc < 3) {
    printf("expected a firmware image and a binary device tree\n");
    exit(EXIT_FAILURE);
  }

  /* 初始化机器 */
  memset(&m, 0, sizeof(m));
  m.ram = malloc(MACH_RAM_SIZE);
  m.cpu = &cpu;
  memset(m.ram, 0, MACH_RAM_SIZE);

  /* 外设设置 */
  rv_init(&cpu, &m, &mach_bus);
  rv_plic_init(&m.plic0);
  rv_clint_init(&m.clint0, &cpu);
  rv_uart_init(&m.uart0, NULL, &uart0_io);
  rv_uart_init(&m.uart1, &m, &uart1_io);

  /* 加载内核与 dtb */
  load(argv[1], m.ram, MACH_RAM_SIZE);
  load(argv[2], m.ram + MACH_DTB_OFFSET, MACH_RAM_SIZE - MACH_DTB_OFFSET);

  /* 试着算出要执行多少条指令 */
  if (argc == 4) {
    ninst = (size_t)atol(argv[3]);
  }

  /* ncurses 设置 */
  initscr();              /* 初始化屏幕 */
  cbreak();               /* 不对输入字符做缓冲 */
  noecho();               /* 不回显输入字符 */
  scrollok(stdscr, TRUE); /* 允许屏幕自动滚动 */
  nodelay(stdscr, TRUE);  /* 启用非阻塞输入 */

  /* 【手册】开机时的寄存器约定 —— 这是"谁把参数交给谁"的接口：
   *   a0 = hartid（硬件线程号，单核固定 0；对应 CSR mhartid，见 M-2.6）
   *   a1 = 设备树（DTB）的物理地址
   * 模拟器准备好这两个寄存器后，PC 从复位向量开始执行 —— 但它跳到的
   * 【第一个程序不是 Linux，而是 OpenSBI】。
   * 完整启动链见 S-11.1「启动的整体流程」与 S-11.3「M 模式到 S 模式的跳转代码」：
   *   ROM/模拟器 → OpenSBI（M 模式）→ Linux（S 模式）→ 用户程序（U 模式）。
   * 每一级初始化完自己的资源后，用 MRET 下降到下一级（M-4.3）。 */
  cpu.r[10] /* a0 */ = 0;                               /* hartid */
  cpu.r[11] /* a1 */ = MACH_RAM_BASE + MACH_DTB_OFFSET; /* dtb 指针 */
  do {
    /* 【手册】时间推进 —— 模拟"墙上时钟"。
     *   mtime / mtimeh 是 CLINT 的 64 位计时器（低 / 高 32 位），
     *   每 0x1000 次主循环加一；它是 Linux 时间片调度的最终来源。
     *   Linux 通过 SBI 设置 mtimecmp，当 mtime ≥ mtimecmp 时 CLINT 产生
     *   定时器中断（MTIP），见 M-13.2「实现 CLINT 定时器中断」。 */
    rv_u32 irq = 0;
    if (!(rtc_period = (rtc_period + 1) & 0xFFF))
      if (!++cpu.csr.mtime)
        cpu.csr.mtimeh++;
    /* 执行一条指令。注意此时 CPU 可能正处于 S 模式跑 Linux —— 
     * "机器"与"核"的边界就在这里：核只管执行，机器负责喂事件。 */
    rv_step(&cpu);
    /* UART 每处理完一个字节就向 PLIC 报一次中断请求（源 1 = uart0，源 2 = uart1，
     * 与 rv.dts 里两个 serial 节点的 interrupts 属性一一对应）。 */
    if (rv_uart_update(&m.uart0))
      rv_plic_irq(&m.plic0, 1);
    if (rv_uart_update(&m.uart1))
      rv_plic_irq(&m.plic0, 2);
    /* 收集三类中断源并注入 CPU：
     *   CLINT 软件中断（MSIP）→ Linux 的多核 IPI、以及 OpenSBI 的调试中断
     *   CLINT 定时器中断（MTIP）→ 时间片调度
     *   PLIC 外部中断（SEIP）→ UART 等外设
     * 乘法系数 RV_CSI/CTI/CEI 是 rv.h 里定义的 mip 位掩码，
     * 详见 rv.c 的 rv_irq 与 M-2.2「mie / mip」。 */
    irq = RV_CSI * rv_clint_msi(&m.clint0, 0) |
          RV_CTI * rv_clint_mti(&m.clint0, 0) |
          RV_CEI * rv_plic_mei(&m.plic0, 0);
    rv_irq(&cpu, irq);
  } while (!ninst || ctr++ < ninst);

  endwin();
  return EXIT_SUCCESS;
}
