/* RV32I[MAC] 模拟器。
 * 参见：https://github.com/riscv/riscv-isa-manual */
#ifndef MN_RV_H
#define MN_RV_H

#define RV_VERSION "1.0.1"

/* 异常列表。 */
#define RV_EIALIGN 0  /* 指令地址对齐异常。 */
#define RV_EIFAULT 1  /* 取指访问异常。 */
#define RV_EILL 2     /* 非法指令异常。 */
#define RV_EBP 3      /* 断点。 */
#define RV_ELALIGN 4  /* 加载地址对齐异常。 */
#define RV_ELFAULT 5  /* 加载访问异常。 */
#define RV_ESALIGN 6  /* 存储地址对齐异常。 */
#define RV_ESFAULT 7  /* 存储访问异常。 */
#define RV_EUECALL 8  /* 来自 U 模式的环境调用。 */
#define RV_ESECALL 9  /* 来自 S 模式的环境调用。 */
#define RV_EMECALL 11 /* 来自 M 模式的环境调用。 */
#define RV_EIPAGE 12  /* 指令缺页异常。 */
#define RV_ELPAGE 13  /* 加载缺页异常。 */
#define RV_ESPAGE 15  /* 存储缺页异常。 */

#if __STDC__ && __STDC_VERSION__ >= 199901L /* 尝试加载 stdint.h。 */
#include <stdint.h>
#define RV_U8_TYPE uint8_t   /* 是的，我知道这些类型都是可选的。 */
#define RV_U16_TYPE uint16_t /* 它们*通常*都存在。无论如何，rv 并不是 */
#define RV_S32_TYPE int32_t  /* 为了运行在 CHAR_BIT != 8 或其它 */
#define RV_U32_TYPE uint32_t /* 奇怪整数规格的系统上而写的。 */
#else
#ifdef __UINT8_TYPE__ /* 既然这里有这些宏，那我们不妨就用它们。 */
#define RV_U8_TYPE __UINT8_TYPE__
#define RV_U16_TYPE __UINT16_TYPE__
#define RV_S32_TYPE __INT32_TYPE__
#define RV_U32_TYPE __UINT32_TYPE__
#else
#define RV_U8_TYPE unsigned char   /* 假设：CHAR_BIT == 8 */
#define RV_U16_TYPE unsigned short /* 假设：sizeof(ushort) == 2 */
#define RV_S32_TYPE signed int     /* 假设：sizeof(sint) == 4 */
#define RV_U32_TYPE unsigned int   /* 假设：sizeof(uint) == 4 */
#endif /* （轻微）偏离 c89。抱歉了，{TI, Cray, DEC 等公司}。 */
#endif /* 我想要的圣诞礼物就是带 stdint.h 的 C89 */

typedef RV_U8_TYPE rv_u8;
typedef RV_U16_TYPE rv_u16;
typedef RV_S32_TYPE rv_s32;
typedef RV_U32_TYPE rv_u32;

/* 结果类型：{RV_OK, RV_BAD, RV_PAGEFAULT, RV_BAD_ALIGN} 之一 */
typedef rv_u32 rv_res;

#define RV_OK 0
#define RV_BAD 1
#define RV_BAD_ALIGN 2
#define RV_PAGEFAULT 3
#define RV_TRAP_NONE 0x80000010
#define RV_TRAP_WFI 0x80000011

typedef struct rv_csr {
  rv_u32 /* sstatus, */ sie, stvec, scounteren, sscratch, sepc, scause, stval,
      sip, satp;
  rv_u32 mstatus, misa, medeleg, mideleg, mie, mtvec, mcounteren, mstatush,
      mscratch, mepc, mcause, mtval, mip, mtime, mtimeh, mvendorid, marchid,
      mimpid, mhartid;
  rv_u32 cycle, cycleh;
} rv_csr;

typedef enum rv_priv { RV_PUSER = 0, RV_PSUPER = 1, RV_PMACH = 3 } rv_priv;
typedef enum rv_access { RV_AR = 1, RV_AW = 2, RV_AX = 4 } rv_access;
typedef enum rv_cause { RV_CSI = 8, RV_CTI = 128, RV_CEI = 512 } rv_cause;

/* 内存访问回调：data 既是输入也是输出，出错时返回 RV_BAD。
 * 访问总是按 `width` 对齐。 */
typedef rv_res (*rv_bus_cb)(void *user, rv_u32 addr, rv_u8 *data,
                            rv_u32 is_store, rv_u32 width);

typedef struct rv {
  rv_bus_cb bus_cb;
  void *user;
  rv_u32 r[32];          /* 通用寄存器 */
  rv_u32 pc;             /* 程序计数器 */
  rv_u32 next_pc;        /* 下一周期的程序计数器 */
  rv_csr csr;            /* csr 状态 */
  rv_u32 priv;           /* 当前特权级 */
  rv_u32 res, res_valid; /* lr/sc 保留集 */
  rv_u32 tlb_va, tlb_pte, tlb_valid, tlb_i;
} rv;

/* 初始化 CPU。可以对 `cpu` 再次调用以复位。 */
void rv_init(rv *cpu, void *user, rv_bus_cb bus_cb);

/* 单步执行 CPU。发生 trap 时返回 trap 原因，否则返回 `RV_TRAP_NONE` */
rv_u32 rv_step(rv *cpu);

/* 触发中断。 */
void rv_irq(rv *cpu, rv_cause cause);

/* 在主机字节序与 LE 之间转换的工具函数。 */
void rv_endcvt(rv_u8 *in, rv_u8 *out, rv_u32 width, rv_u32 is_store);

#endif

/* Copyright (c) 2023 Max Nurzia

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the “Software”), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. */
