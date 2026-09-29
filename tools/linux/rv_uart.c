/* ============================================================================
 * rv_uart.c —— SiFive UART（串口 / 控制台）
 *
 * 【它负责什么】模拟 SiFive FE310 的 UART：
 *   · 8 字节深度的收发 FIFO（RV_UART_FIFO_SIZE）
 *   · 按 div 分频模拟波特率：每 div 个时钟周期收 / 发一个字节
 *   · 收 / 发达到阀值（txctrl / rxctrl 的高 16 位）时置 ip 位，进而向 PLIC
 *     提出中断请求（源 1 = uart0、源 2 = uart1，与 rv.dts 里对应）
 *
 * 【与手册的关系】UART 属于平台外设，不在 RISC-V 特权手册范围内。它和
 *   特权架构只有两个接触点：
 *     · 中断最终进入 mip.SEIP（见 rv.c 的 rv_irq 与 M-2.2）
 *     · Linux 侧要么直接 MMIO 访问它，要么通过 SBI console 调用间接使用
 *   设备树里的 compatible = "sifive,uart0" 决定内核加载哪个驱动。
 * ============================================================================ */

#include "rv_uart.h"

#include <string.h>

void rv_uart_fifo_init(rv_uart_fifo *fifo) { memset(fifo, 0, sizeof(*fifo)); }

void rv_uart_fifo_put(rv_uart_fifo *fifo, rv_u8 byte) {
  if (fifo->size == RV_UART_FIFO_SIZE)
    return;
  fifo->buf[(fifo->read + fifo->size) & (RV_UART_FIFO_SIZE - 1)] = byte;
  fifo->size++;
}

rv_u8 rv_uart_fifo_get(rv_uart_fifo *fifo) {
  rv_u8 ch = fifo->buf[fifo->read];
  if (!fifo->size)
    return 0;
  fifo->read = (fifo->read + 1) & (RV_UART_FIFO_SIZE - 1);
  fifo->size--;
  return ch;
}

void rv_uart_init(rv_uart *uart, void *user, rv_uart_cb cb) {
  memset(uart, 0, sizeof(*uart));
  uart->user = user;
  uart->cb = cb;
  uart->div = 3;
  rv_uart_fifo_init(&uart->tx);
  rv_uart_fifo_init(&uart->rx);
}

/* 【寄存器表】SiFive UART 的布局（见 rv_uart.h 与 SiFive FE310 手册第 17 章）：
 *     0x00 txdata   写：发送一个字节；读：bit31 = TX FIFO 满
 *     0x04 rxdata   读：bit31 = RX FIFO 空，低 8 位是收到的字节
 *     0x08 txctrl   bit0 = 发送使能；bit19:16 = TX 中断阀值
 *     0x0C rxctrl   bit0 = 接收使能；bit19:16 = RX 中断阀值
 *     0x10 ie       bit0 = TX 中断使能，bit1 = RX 中断使能
 *     0x14 ip       bit0 = TX 中断挂起，bit1 = RX 中断挂起（只读）
 *     0x18 div      波特率分频值
 *     0x1C 未使用
 *
 * 这些位与 PLIC 的联动点看 rv_uart_update() 的末尾：当 FIFO 水位越过
 * txctrl/rxctrl 里的阀值、且 ie 相应位使能时，ip 被置位并返回 1，
 * mach.c 随即调用 rv_plic_irq() 把它上报给 PLIC。
 */
rv_res rv_uart_bus(rv_uart *uart, rv_u32 addr, rv_u8 *d, rv_u32 is_store,
                   rv_u32 width) {
  rv_u32 data;
  rv_endcvt(d, (rv_u8 *)&data, 4, 0);
  if (width != 4)
    return RV_BAD_ALIGN;
  if (addr == 0x00) { /*R txdata */
    if (is_store)
      rv_uart_fifo_put(&uart->tx, (rv_u8)data);
    else
      data = (rv_u32)(uart->tx.size == RV_UART_FIFO_SIZE) << 31U;
  } else if (addr == 0x04) { /*R rxdata */
    if (!is_store)
      data = ((rv_u32)(!uart->rx.size) << 31U) | rv_uart_fifo_get(&uart->rx);
  } else if (addr == 0x08) { /*R txctrl */
    if (is_store)
      uart->txctrl = data & 0x00070003; /* 不支持 nstop */
    else
      data = uart->txctrl;
  } else if (addr == 0x0C) { /*R rxctrl */
    if (is_store)
      uart->rxctrl = data & 0x00070001;
    else
      data = uart->rxctrl;
  } else if (addr == 0x10) { /*R ie */
    if (is_store)
      uart->ie = data & 3;
    else
      data = uart->ie;
  } else if (addr == 0x14) { /*R ip */
    if (!is_store)
      data = uart->ip;
  } else if (addr == 0x18) { /*R div */
    if (is_store)
      uart->div = data & 0xFFFF;
    else
      data = uart->div;
  } else if (addr == 0x1C) { /*R 未使用 */
    if (!is_store)
      data = 0;
  } else {
    return RV_BAD;
  }
  rv_endcvt((rv_u8 *)&data, d, 4, 1);
  return RV_OK;
}

rv_u32 rv_uart_update(rv_uart *uart) {
  rv_u8 byte = uart->tx.buf[uart->tx.read];
  if (++uart->clk >= uart->div) {
    if ((uart->txctrl & 1) && uart->tx.size &&
        (uart->cb(uart->user, &byte, 1) == RV_OK))
      rv_uart_fifo_get(&uart->tx);
    if ((uart->rxctrl & 1) && (uart->rx.size < RV_UART_FIFO_SIZE) &&
        (uart->cb(uart->user, &byte, 0) == RV_OK))
      rv_uart_fifo_put(&uart->rx, byte);
    uart->clk = 0;
  }
  if ((uart->txctrl >> 16) > uart->tx.size && (uart->ie & 1))
    uart->ip |= 1;
  else
    uart->ip &= ~(1U);
  if ((uart->rxctrl >> 16) < uart->rx.size && (uart->ie & 2))
    uart->ip |= 2;
  else
    uart->ip &= ~(2U);
  return !!uart->ip;
}
