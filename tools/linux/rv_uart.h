/* SiFive 通用异步收发器实现
 * 参见：SiFive FE310-G003 手册，第 17 章 */

#ifndef RV_UART_H
#define RV_UART_H

#include "rv.h"

/* UART I/O 回调，*byte 即数据。与总线访问回调类似。 */
typedef rv_res (*rv_uart_cb)(void *user, rv_u8 *byte, rv_u32 is_write);

#define RV_UART_FIFO_SIZE 8U

/* UART 使用的内部 FIFO 实例 */
typedef struct rv_uart_fifo {
  rv_u8 buf[RV_UART_FIFO_SIZE];
  rv_u32 read, size;
} rv_uart_fifo;

typedef struct rv_uart {
  rv_uart_cb cb;
  void *user;
  rv_uart_fifo rx, tx;
  rv_u32 txctrl, rxctrl, ip, ie, div, clk;
} rv_uart;

/* 用用户提供的 I/O 回调初始化一个 UART */
void rv_uart_init(rv_uart *uart, void *user, rv_uart_cb cb);

#define RV_UART_SIZE /* 内存映射大小 */ 0x20

/* 对 UART 执行一次总线访问 */
rv_res rv_uart_bus(rv_uart *uart, rv_u32 addr, rv_u8 *data, rv_u32 is_store,
                   rv_u32 width);

/* 更新 UART */
rv_u32 rv_uart_update(rv_uart *uart);

#endif
