#include <curses.h>
#include <stdlib.h>
#include <string.h>

#include "rv.h"
#include "rv_clint.h"
#include "rv_plic.h"
#include "rv_uart.h"

#define MACH_RAM_BASE 0x80000000UL
#define MACH_RAM_SIZE (1024UL * 1024UL * 128UL) /* 128MiB 内存 */
#define MACH_DTB_OFFSET 0x2000000UL             /* dtb 位于 32MiB 处 */

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

  /* bootloader 和 linux 期望如下： */
  cpu.r[10] /* a0 */ = 0;                               /* hartid */
  cpu.r[11] /* a1 */ = MACH_RAM_BASE + MACH_DTB_OFFSET; /* dtb 指针 */
  do {
    rv_u32 irq = 0;
    if (!(rtc_period = (rtc_period + 1) & 0xFFF))
      if (!++cpu.csr.mtime)
        cpu.csr.mtimeh++;
    rv_step(&cpu);
    if (rv_uart_update(&m.uart0))
      rv_plic_irq(&m.plic0, 1);
    if (rv_uart_update(&m.uart1))
      rv_plic_irq(&m.plic0, 2);
    irq = RV_CSI * rv_clint_msi(&m.clint0, 0) |
          RV_CTI * rv_clint_mti(&m.clint0, 0) |
          RV_CEI * rv_plic_mei(&m.plic0, 0);
    rv_irq(&cpu, irq);
  } while (!ninst || ctr++ < ninst);

  endwin();
  return EXIT_SUCCESS;
}
