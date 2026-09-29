#include "rv.h"

#include <string.h>

#define RV_RESET_VEC 0x80000000 /* CPU 复位向量 */

#define rv_ext(c) (1 << (rv_u8)((c) - 'A')) /* misa 中的 ISA 扩展位 */

void rv_init(rv *cpu, void *user, rv_bus_cb bus_cb) {
  memset(cpu, 0, sizeof(*cpu));
  cpu->user = user;
  cpu->bus_cb = bus_cb;
  cpu->pc = RV_RESET_VEC;
  cpu->csr.misa = (1 << 30)     /* MXL = 1 [XLEN=32] */
                  | rv_ext('A') /* 原子指令 */
                  | rv_ext('S') /* 监督者模式 */
                  | rv_ext('U') /* 用户模式 */;
  cpu->priv = RV_PMACH;
}

/* 将 x 从第 h 位起做符号扩展 */
static rv_u32 rv_signext(rv_u32 x, rv_u32 h) { return (0 - (x >> h)) << h | x; }

#define RV_SBIT 0x80000000                    /* 符号位 */
#define rv_sgn(x) (!!((rv_u32)(x) & RV_SBIT)) /* 取出符号 */

/* 计算溢出 */
#define rv_ovf(a, b, y) ((((a) ^ (b)) & RV_SBIT) && (((y) ^ (a)) & RV_SBIT))

#define rv_bf(i, h, l)                                                         \
  (((i) >> (l)) & ((1 << ((h) - (l) + 1)) - 1))    /* 提取位域 */
#define rv_b(i, l) rv_bf(i, l, l)                  /* 提取单比特 */
#define rv_tb(i, l, o) (rv_b(i, l) << (o))         /* 平移单比特 */
#define rv_tbf(i, h, l, o) (rv_bf(i, h, l) << (o)) /* 平移位域 */

/* 指令字段宏 */
#define rv_ioph(i) rv_bf(i, 6, 5)                     /* 操作码高位 */
#define rv_iopl(i) rv_bf(i, 4, 2)                     /* 操作码低位 */
#define rv_if3(i) rv_bf(i, 14, 12)                    /* funct3 */
#define rv_if5(i) rv_bf(i, 31, 27)                    /* funct5 */
#define rv_if7(i) rv_bf(i, 31, 25)                    /* funct7 */
#define rv_ird(i) rv_bf(i, 11, 7)                     /* rd */
#define rv_irs1(i) rv_bf(i, 19, 15)                   /* rs1 */
#define rv_irs2(i) rv_bf(i, 24, 20)                   /* rs2 */
#define rv_iimm_i(i) rv_signext(rv_bf(i, 31, 20), 11) /* I 型立即数 */
#define rv_iimm_iu(i) rv_bf(i, 31, 20) /* I 型零扩展立即数 */
#define rv_iimm_s(i)                                                           \
  (rv_signext(rv_tbf(i, 31, 25, 5), 11) | rv_tbf(i, 30, 25, 5) |               \
   rv_bf(i, 11, 7))                        /* S 型立即数 */
#define rv_iimm_u(i) rv_tbf(i, 31, 12, 12) /* U 型立即数 */
#define rv_iimm_b(i)                                                           \
  (rv_signext(rv_tb(i, 31, 12), 12) | rv_tb(i, 7, 11) | rv_tbf(i, 30, 25, 5) | \
   rv_tbf(i, 11, 8, 1)) /* B 型立即数 */
#define rv_iimm_j(i)                                                           \
  (rv_signext(rv_tb(i, 31, 20), 20) | rv_tbf(i, 19, 12, 12) |                  \
   rv_tb(i, 20, 11) | rv_tbf(i, 30, 21, 1))     /* J 型立即数 */
#define rv_isz(i) (rv_bf(i, 1, 0) == 3 ? 4 : 2) /* 指令长度 */

/* 读寄存器 */
static rv_u32 rv_lr(rv *cpu, rv_u8 i) { return cpu->r[i]; }

/* 写寄存器 */
static void rv_sr(rv *cpu, rv_u8 i, rv_u32 v) { cpu->r[i] = i ? v : 0; }

#define RV_CSR(num, r, w, dst) /* 检查我们是否正在访问 csr `num` */           \
  y = ((csr == (num)) ? (rm = r, wm = w, &cpu->csr.dst) : y)

/* csr 总线访问 —— 我们把 csr 建模为一条内部存储器总线 */
static rv_res rv_csr_bus(rv *cpu, rv_u32 csr, rv_u32 w, rv_u32 *io) {
  rv_u32 *y = NULL /* 物理寄存器 */, wm /* 可写位 */ = -1U, rm = -1U;
  rv_u32 rw = rv_bf(csr, 11, 10), priv = rv_bf(csr, 9, 8);
  if ((w && rw == 3) || cpu->priv < priv ||
      (csr == 0x180 && cpu->priv == RV_PSUPER && rv_b(cpu->csr.mstatus, 20)))
    return RV_BAD; /* 非法访问，或在 tvm=1 时写 satp */
  /*     id     读掩码      写掩码      物理寄存器        csr 名 */
  RV_CSR(0x100, 0x800DE762, 0x800DE762, mstatus);    /*C sstatus */
  RV_CSR(0x104, 0x00000222, 0x00000222, mie);        /*C sie */
  RV_CSR(0x105, 0xFFFFFFFF, 0xFFFFFFFF, stvec);      /*C stvec */
  RV_CSR(0x106, 0xFFFFFFFF, 0x00000000, scounteren); /*C scounteren */
  RV_CSR(0x140, 0xFFFFFFFF, 0xFFFFFFFF, sscratch);   /*C sscratch */
  RV_CSR(0x141, 0xFFFFFFFF, 0xFFFFFFFF, sepc);       /*C sepc */
  RV_CSR(0x142, 0xFFFFFFFF, 0xFFFFFFFF, scause);     /*C scause */
  RV_CSR(0x143, 0xFFFFFFFF, 0xFFFFFFFF, stval);      /*C stval */
  RV_CSR(0x144, 0x00000222, 0x00000222, sip);        /*C sip */
  RV_CSR(0x180, 0xFFFFFFFF, 0xFFFFFFFF, satp);       /*C satp */
  RV_CSR(0x300, 0x807FFFEC, 0x807FFFEC, mstatus);    /*C mstatus */
  RV_CSR(0x301, 0xFFFFFFFF, 0x00000000, misa);       /*C misa */
  RV_CSR(0x302, 0xFFFFFFFF, 0xFFFFFFFF, medeleg);    /*C medeleg */
  RV_CSR(0x303, 0xFFFFFFFF, 0xFFFFFFFF, mideleg);    /*C mideleg */
  RV_CSR(0x304, 0xFFFFFFFF, 0x00000AAA, mie);        /*C mie */
  RV_CSR(0x305, 0xFFFFFFFF, 0xFFFFFFFF, mtvec);      /*C mtvec */
  RV_CSR(0x306, 0xFFFFFFFF, 0x00000000, mcounteren); /*C mcounteren */
  RV_CSR(0x310, 0x00000030, 0x00000030, mstatush);   /*C mstatush */
  RV_CSR(0x340, 0xFFFFFFFF, 0xFFFFFFFF, mscratch);   /*C mscratch */
  RV_CSR(0x341, 0xFFFFFFFF, 0xFFFFFFFF, mepc);       /*C mepc */
  RV_CSR(0x342, 0xFFFFFFFF, 0xFFFFFFFF, mcause);     /*C mcause */
  RV_CSR(0x343, 0xFFFFFFFF, 0x00000000, mtval);      /*C mtval */
  RV_CSR(0x344, 0xFFFFFFFF, 0x00000AAA, mip);        /*C mip */
  RV_CSR(0xC00, 0xFFFFFFFF, 0xFFFFFFFF, cycle);      /*C cycle */
  RV_CSR(0xC01, 0xFFFFFFFF, 0xFFFFFFFF, mtime);      /*C time */
  RV_CSR(0xC02, 0xFFFFFFFF, 0xFFFFFFFF, cycle);      /*C instret */
  RV_CSR(0xC80, 0xFFFFFFFF, 0xFFFFFFFF, cycleh);     /*C cycleh */
  RV_CSR(0xC81, 0xFFFFFFFF, 0xFFFFFFFF, mtimeh);     /*C timeh */
  RV_CSR(0xC82, 0xFFFFFFFF, 0xFFFFFFFF, cycleh);     /*C instreth */
  RV_CSR(0xF11, 0xFFFFFFFF, 0x00000000, mvendorid);  /*C mvendorid */
  RV_CSR(0xF12, 0xFFFFFFFF, 0x00000000, marchid);    /*C marchid */
  RV_CSR(0xF13, 0xFFFFFFFF, 0x00000000, mimpid);     /*C mimpid */
  RV_CSR(0xF14, 0xFFFFFFFF, 0xFFFFFFFF, mhartid);    /*C mhartid */
  if (!y)
    return RV_BAD;                       /* 非法 csr */
  *io = w ? *io : (*y & rm);             /* 只读取允许读的位 */
  *y = w ? (*y & ~wm) | (*io & wm) : *y; /* 只写入允许写的位 */
  return RV_OK;
}

/* 触发一次 trap */
static rv_u32 rv_trap(rv *cpu, rv_u32 cause, rv_u32 tval) {
  rv_u32 is_interrupt = !!(cause & 0x80000000), rcause = cause & ~0x80000000;
  rv_priv xp = /* 目标特权级，由 y = cpu->priv 切换到它 */
      (cpu->priv < RV_PMACH) &&
              ((is_interrupt ? cpu->csr.mideleg : cpu->csr.medeleg) &
               (1 << rcause))
          ? RV_PSUPER
          : RV_PMACH;
  rv_u32 *xtvec = &cpu->csr.mtvec, *xepc = &cpu->csr.mepc,
         *xcause = &cpu->csr.mcause, *xtval = &cpu->csr.mtval;
  rv_u32 xie = rv_b(cpu->csr.mstatus, xp);
  if (xp == RV_PSUPER) /* 选择 S 模式寄存器 */
    xtvec = &cpu->csr.stvec, xepc = &cpu->csr.sepc, xcause = &cpu->csr.scause,
    xtval = &cpu->csr.stval;
  cpu->csr.mstatus &= (xp == RV_PMACH ? 0xFFFFE777   /* {mpp, mie, mpie} <- 0 */
                                      : 0xFFFFFEDD); /* {spp, sie, spie} <- 0 */
  cpu->csr.mstatus |= (cpu->priv << (xp == RV_PMACH ? 11 : 8)) /* xpp <- y */
                      | xie << (4 + xp);                       /* xpie <- xie */
  *xepc = cpu->pc;                                             /* xepc <- pc */
  *xcause = rcause | (is_interrupt << 31); /* xcause <- cause */
  *xtval = tval;                           /* xtval <- tval */
  cpu->priv = xp;                          /* priv <- x */
  /* 若 tvec[0] 为 1，返回 4 * cause + vec，否则仅返回 vec */
  cpu->pc = (*xtvec & ~3U) + 4 * rcause * ((*xtvec & 1) && is_interrupt);
  return cause;
}

/* 带 tval 的总线访问 trap */
static rv_u32 rv_trap_bus(rv *cpu, rv_u32 err, rv_u32 tval, rv_access a) {
  static const rv_u32 ex[] = {RV_EIFAULT, RV_EIALIGN, RV_EIPAGE,  /* RV_AR */
                              RV_ELFAULT, RV_ELALIGN, RV_ELPAGE,  /* RV_AW */
                              RV_ESFAULT, RV_ESALIGN, RV_ESPAGE}; /* RV_AX */
  return rv_trap(cpu, ex[(a == RV_AW ? 2 : a == RV_AR) * 3 + err - 1], tval);
}

/* sv32 虚拟地址 -> 物理地址 */
static rv_u32 rv_vmm(rv *cpu, rv_u32 va, rv_u32 *pa, rv_access access) {
  rv_u32 epriv = rv_b(cpu->csr.mstatus, 17) && access != RV_AX
                     ? rv_bf(cpu->csr.mstatus, 12, 11)
                     : cpu->priv; /* 有效特权模式 */
  if (!rv_b(cpu->csr.satp, 31) || epriv > RV_PSUPER) {
    *pa = va; /* 若 !satp.mode，则不做地址转换 */
  } else {
    rv_u32 ppn /* satp.ppn */ = rv_bf(cpu->csr.satp, 21, 0),
               a /* satp.ppn * PAGESIZE */ = ppn << 12, i /* LEVELS - 1 */ = 1,
               pte, pte_address, tlb_hit = 0;
    if (cpu->tlb_valid && cpu->tlb_va == (va & ~0xFFFU))
      pte = cpu->tlb_pte, tlb_hit = 1, i = cpu->tlb_i;
    while (!tlb_hit) {
      /* pte_address = a + va.vpn[i] * PTESIZE */
      pte_address = a + (rv_bf(va, 21 + 10 * i, 12 + 10 * i) << 2);
      if (cpu->bus_cb(cpu->user, pte_address, (rv_u8 *)&pte, 0, 4))
        return RV_BAD;
      rv_endcvt((rv_u8 *)&pte, (rv_u8 *)&pte, 4, 0);
      if (!rv_b(pte, 0) || (!rv_b(pte, 1) && rv_b(pte, 2)))
        return RV_PAGEFAULT; /* pte.v == 0，或 (pte.r == 0 且 pte.w == 1) */
      if (rv_b(pte, 1) || rv_b(pte, 3))
        break; /* 若 pte.r = 1 或 pte.x = 1，说明这是叶子页 */
      if (i == 0)
        return RV_PAGEFAULT; /* 若 i - 1 < 0，则缺页 */
      i = i - 1;
      a = rv_tbf(pte, 31, 10, 12); /* a = pte.ppn[*] * PAGESIZE */
    }
    if (!tlb_hit)
      cpu->tlb_va = va & ~0xFFFU, cpu->tlb_pte = pte, cpu->tlb_i = i,
      cpu->tlb_valid = 1; /* 避免下次访问时再次遍历 pte */
    if (rv_b(cpu->csr.mstatus, 19))
      pte |= rv_b(pte, 3) << 2;              /* 若置位 mxr，则 pte.r = pte.x */
    if ((!rv_b(pte, 4) && epriv == RV_PUSER) /* 未设置 u 位 */
        || (epriv == RV_PSUPER && !rv_b(cpu->csr.mstatus, 18) &&
            rv_b(pte, 4))                       /* mstatus.sum 位不正确 */
        || ~rv_bf(pte, 3, 1) & access           /* 访问类型不匹配 */
        || (i && rv_bf(pte, 19, 10))            /* 巨页未对齐 */
        || !rv_b(pte, 6)                        /* pte.a == 0 */
        || ((access & RV_AW) && !rv_b(pte, 7))) /* 写操作且 pte.d == 0 */
      return RV_PAGEFAULT;
    /* pa.ppn[1:i] = pte.ppn[1:i] */
    *pa = rv_tbf(pte, 31, 10 + 10 * i, 12 + 10 * i) | rv_bf(va, 11 + 10 * i, 0);
  }
  return RV_OK;
}

void rv_endcvt(rv_u8 *in, rv_u8 *out, rv_u32 width, rv_u32 is_store) {
  if (!is_store && width == 1)
    *out = in[0];
  else if (!is_store && width == 2)
    *((rv_u16 *)out) = (rv_u16)(in[0] << 0) | (rv_u16)(in[1] << 8);
  else if (!is_store && width == 4)
    *((rv_u32 *)out) = (rv_u32)(in[0] << 0) | (rv_u32)(in[1] << 8) |
                       (rv_u32)(in[2] << 16) | (rv_u32)(in[3] << 24);
  else if (width == 1)
    out[0] = *in;
  else if (width == 2)
    out[0] = *(rv_u16 *)in >> 0 & 0xFF, out[1] = (*(rv_u16 *)in >> 8);
  else
    out[0] = *(rv_u32 *)in >> 0 & 0xFF, out[1] = *(rv_u32 *)in >> 8 & 0xFF,
    out[2] = *(rv_u32 *)in >> 16 & 0xFF, out[3] = *(rv_u32 *)in >> 24 & 0xFF;
}

/* rv_bus —— 所有"需要地址翻译"的内存访问的唯一入口。
 *
 * 取指、load、store、AMO 全都汇聚到这里。它和 bus_cb 的分工是理解
 * 整个 CPU 的关键：
 *
 *     调用者（rv_if 取指 / rv_step 的访存分支）
 *        │  发出的是"虚拟地址"
 *        ▼
 *      rv_bus    ← 对齐检查、地址翻译、字节序转换
 *        │  交给宿主的已经是"物理地址"
 *        ▼
 *      bus_cb    ← 宿主实现的地址解码器（RAM / UART / PLIC / CLINT ...）
 *
 * 参数：
 *   va     [进]    虚拟地址。no-rvc 分支已删除跨页处理，本函数不再修改它。
 *   data   [进/出] RISC-V 语义下的数据缓冲：
 *                  RV_AR / RV_AX 时是输出（把读到的值写回这里），
 *                  RV_AW 时是输入（从这里取出要写的值）。
 *   width  [进]    访问宽度，调用者保证只能是 1、2、4。
 *   access [进]    RV_AR = 读，RV_AW = 写，RV_AX = 取指（执行访问）。
 *
 * 返回值：RV_OK(0) 表示成功；否则是 RV_BAD / RV_BAD_ALIGN / RV_PAGEFAULT，
 *         由调用者交给 rv_trap_bus 换算成 load / store / instruction 三类异常。
 *
 * 两条不可动摇的约定：
 *   1. bus_cb 只认物理地址。所以 rv_vmm 遍历页表时必须直接调用 cpu->bus_cb，
 *      绝不能再走 rv_bus —— 否则就成了"用翻译器去翻译翻译器自己"，会无限
 *      递归直到栈溢出。（rv_vmm 里那一行 cpu->bus_cb 就是递归的终止点。）
 *   2. bus_cb 交换的是"字节流"（rv_u8 数组）而不是整数，因为宿主可能是
 *      大端机。两侧各做一次 rv_endcvt 完成小端 <-> 宿主字节序的换算；
 *      在小端宿主上这两次转换互为逆运算，等于什么都没做。
 */
static rv_u32 rv_bus(rv *cpu, rv_u32 *va, rv_u8 *data, rv_u32 width,
                     rv_access access) {
  rv_u32 err, pa /* 翻译后的物理地址，由下面的 rv_vmm 填写 */;
  rv_u8 ledata[4]; /* 与 bus_cb 交换数据用的临时缓冲，最多 4 字节 */

  /* 步骤 1：把 rv 侧的数据摊平成"宿主表示"的字节序列，装进 ledata。
   * is_store 传 1，表示方向是 "rv 侧 -> 宿主侧"。
   * 先转换的好处是：后面无论要分几次搬运，交给 bus_cb 的都只是 ledata
   * 里的字节，rv 侧的 data 不必反复参与。 */
  rv_endcvt(data, ledata, width, 1);

  /* 步骤 2：对齐检查。这里不翻译地址、也不碰 bus_cb，直接返回错误码。
   * rv_trap_bus 会把它变成 RV_EIALIGN / RV_ELALIGN / RV_ESALIGN（取指/读/写）。
   *
   * no-rvc 分支补充：这条检查还顺手排除了"跨页访问"。因为能走到下面的地址
   * 必然是 width 的倍数、而 4096 也是 width（1/2/4）的倍数，所以一次访问的
   * 首尾字节必然同页 —— 原来那段"跨页拆成两段"的分支已因此删除。 */
  if (*va & (width - 1))
    return RV_BAD_ALIGN;

  /* 步骤 3：虚拟地址 -> 物理地址。
   * rv_vmm 内部在 TLB 未命中时会去读页表，那条路径走的是 cpu->bus_cb
   * 而不是本函数（理由见函数头第 1 条约定）。
   * 失败时 err 可能是 RV_BAD（访问异常）或 RV_PAGEFAULT（缺页）。 */
  if ((err = rv_vmm(cpu, *va, &pa, access)))
    return err; /* 缺页或访问异常 */

  /* 步骤 4：真正的总线访问 —— 全函数唯一把请求交给宿主的地方。
   * 只有 access == RV_AW 时才置 is_store，所以取指（RV_AX）和读（RV_AR）
   * 在 bus_cb 看来都是"读"。返回非 0 时原样向上传递（见步骤 2 的说明）。 */
  if ((err = cpu->bus_cb(cpu->user, pa, ledata, access == RV_AW, width)))
    return err;

  /* 步骤 5：把 bus_cb 填好的字节序列重新组装成 rv 侧的整数，写回 data。
   * is_store 传 0，表示方向是 "宿主侧 -> rv 侧"。 */
  rv_endcvt(ledata, data, width, 0);
  return 0; /* 等价于 RV_OK */
}

/* 取指 */
static rv_u32 rv_if(rv *cpu, rv_u32 *i, rv_u32 *tval) {
  rv_u32 err, pc = cpu->pc;
  /* no-rvc 分支：rv_step 已保证 pc 四字节对齐，而四字节对齐的访问不可能跨页，
   * 所以原来“分两次 2 字节取指”的分支（连同 page 变量）整体删除。 */
  if ((err = rv_bus(cpu, &pc, (rv_u8 *)i, 4, RV_AX))) /* 4 字节取指 */
    goto error;
  cpu->next_pc = cpu->pc + rv_isz(*i);
  *tval = *i; /* 对非法指令 trap 而言，tval 是原始指令 */
  /* no-rvc 分支：不再解压压缩指令。若取到的是 16 位指令，rv_isz(*i) 会返回 2，
   * 由 rv_step 开头的长度检查判为非法指令（RV_EILL）。 */
  return RV_OK;
error:
  *tval = pc; /* 对指令 {page}fault trap 而言，tval 是 pc */
  return err;
}

/* 处理中断 */
static rv_u32 rv_service(rv *cpu) {
  rv_u32 iidx /* 中断号 */, d /* 委派的特权级 */;
  for (iidx = 12; iidx > 0; iidx--) { /* 从最高优先级到最低优先级 */
    if (!(cpu->csr.mip & cpu->csr.mie & (1 << iidx)))
      continue; /* 中断未触发或未使能 */
    d = (cpu->csr.mideleg & (1 << iidx)) ? RV_PSUPER : RV_PMACH;
    if (d == cpu->priv ? rv_b(cpu->csr.mstatus, d) : (d > cpu->priv))
      return rv_trap(cpu, 0x80000000U + iidx, cpu->pc);
  }
  return RV_TRAP_NONE;
}

/* 单步执行 */
rv_u32 rv_step(rv *cpu) {
  rv_u32 i, tval, err;
  if (!++cpu->csr.cycle)
    cpu->csr.cycleh++; /* 带进位地累加到 cycle、cycleh */
  /* no-rvc 分支新增：没有 C 扩展时指令必然 32 位，pc 必须四字节对齐。
   * 规范把这种情况定义为 instruction-address-misaligned（cause 0），
   * mtval 记录出错的地址。jalr 只清 bit 0，因此跳到 0x...002 会在
   * 这里被拦下，而不是被 rv_if 悄悄当成“2 字节对齐的指令”执行。 */
  if (cpu->pc & 3)
    return rv_trap(cpu, RV_EIALIGN, cpu->pc);
  err = rv_if(cpu, &i, &tval); /* 取指令到 i */
  if (err)
    return rv_trap_bus(cpu, err, tval, RV_AX); /* 取指错误 */
  if (rv_isz(i) != 4)
    return rv_trap(cpu, RV_EILL, tval); /* 指令长度非法 */
  if (rv_iopl(i) == 0) {
    if (rv_ioph(i) == 0) { /*Q 00/000: LOAD */
      rv_u32 va /* 虚拟地址 */ = rv_lr(cpu, rv_irs1(i)) + rv_iimm_i(i);
      rv_u32 v /* 加载的值 */ = 0, w /* 值宽度 */, sx /* 符号扩展 */;
      w = 1 << (rv_if3(i) & 3), sx = ~rv_if3(i) & 4; /*I lb, lh, lw, lbu, lhu */
      if ((err = rv_bus(cpu, &va, (rv_u8 *)&v, w, RV_AR)))
        return rv_trap_bus(cpu, err, va, RV_AR);
      if ((rv_if3(i) & 3) == 3)
        return rv_trap(cpu, RV_EILL, tval); /* 不支持 ld 指令 */
      if (sx)
        v = rv_signext(v, (w * 8 - 1));
      rv_sr(cpu, rv_ird(i), v);
    } else if (rv_ioph(i) == 1) { /*Q 01/000: STORE */
      rv_u32 va /* 虚拟地址 */ = rv_lr(cpu, rv_irs1(i)) + rv_iimm_s(i);
      rv_u32 w /* 值宽度 */ = 1 << (rv_if3(i) & 3);
      rv_u32 y /* 要存储的值 */ = rv_lr(cpu, rv_irs2(i));
      if (rv_if3(i) > 2)                    /*I sb, sh, sw */
        return rv_trap(cpu, RV_EILL, tval); /* 不支持 sd 指令 */
      if ((err = rv_bus(cpu, &va, (rv_u8 *)&y, w, RV_AW)))
        return rv_trap_bus(cpu, err, va, RV_AW);
    } else if (rv_ioph(i) == 3) { /*Q 11/000: BRANCH */
      rv_u32 a = rv_lr(cpu, rv_irs1(i)), b = rv_lr(cpu, rv_irs2(i));
      rv_u32 y /* 比较值 */ = a - b;
      rv_u32 zero = !y, sgn = rv_sgn(y), ovf = rv_ovf(a, b, y), carry = y > a;
      rv_u32 targ = cpu->pc + rv_iimm_b(i);   /* 计算出的跳转目标 */
      if ((rv_if3(i) == 0 && zero) ||         /*I beq */
          (rv_if3(i) == 1 && !zero) ||        /*I bne */
          (rv_if3(i) == 4 && (sgn != ovf)) || /*I blt */
          (rv_if3(i) == 5 && (sgn == ovf)) || /*I bge */
          (rv_if3(i) == 6 && carry) ||        /*I bltu */
          (rv_if3(i) == 7 && !carry)          /*I bgtu */
      ) {
        cpu->next_pc = targ; /* 执行跳转 */
      } else if (rv_if3(i) == 2 || rv_if3(i) == 3)
        return rv_trap(cpu, RV_EILL, tval);
      /* 默认：不跳转 [顺序执行到这里] */
    } else
      return rv_trap(cpu, RV_EILL, tval);
  } else if (rv_iopl(i) == 1) {
    if (rv_ioph(i) == 3 && rv_if3(i) == 0) { /*Q 11/001: JALR */
      rv_u32 target = (rv_lr(cpu, rv_irs1(i)) + rv_iimm_i(i)); /*I jalr */
      rv_sr(cpu, rv_ird(i), cpu->next_pc);
      cpu->next_pc = target & ~1U; /* 目标地址按两字节对齐 */
    } else
      return rv_trap(cpu, RV_EILL, tval);
  } else if (rv_iopl(i) == 3) {
    if (rv_ioph(i) == 0) {            /*Q 00/011: MISC-MEM */
      if (rv_if3(i) == 0) {           /*I fence */
        rv_u32 fm = rv_bf(i, 31, 28); /* 提取 fm 字段 */
        if (fm && fm != 8)
          return rv_trap(cpu, RV_EILL, tval);
      } else if (rv_if3(i) == 1) { /*I fence.i */
      } else
        return rv_trap(cpu, RV_EILL, tval);
    } else if (rv_ioph(i) == 1) { /*Q 01/011: AMO */
      rv_u32 va /* 地址 */ = rv_lr(cpu, rv_irs1(i));
      rv_u32 b /* 参数 */ = rv_lr(cpu, rv_irs2(i));
      rv_u32 x /* 加载的值 */ = 0, y /* 要存储的值 */ = b;
      rv_u32 l /* 是否应加载？ */ = rv_if5(i) != 3, s /* 是否应存储？ */ = 1;
      if (rv_bf(i, 14, 12) != 2) { /* 宽度必须为 2 */
        return rv_trap(cpu, RV_EILL, tval);
      } else {
        if (l && (err = rv_bus(cpu, &va, (rv_u8 *)&x, 4, RV_AR)))
          return rv_trap_bus(cpu, err, va, RV_AR);
        if (rv_if5(i) == 0) /*I amoadd.w */
          y = x + b;
        else if (rv_if5(i) == 1) /*I amoswap.w */
          y = b;
        else if (rv_if5(i) == 2 && !b) /*I lr.w */
          cpu->res = va, cpu->res_valid = 1, s = 0;
        else if (rv_if5(i) == 3) /*I sc.w */
          x = !(cpu->res_valid && cpu->res_valid-- && cpu->res == va), s = !x;
        else if (rv_if5(i) == 4) /*I amoxor.w */
          y = x ^ b;
        else if (rv_if5(i) == 8) /*I amoor.w */
          y = x | b;
        else if (rv_if5(i) == 12) /*I amoand.w */
          y = x & b;
        else if (rv_if5(i) == 16) /*I amomin.w */
          y = rv_sgn(x - b) != rv_ovf(x, b, x - b) ? x : b;
        else if (rv_if5(i) == 20) /*I amomax.w */
          y = rv_sgn(x - b) == rv_ovf(x, b, x - b) ? x : b;
        else if (rv_if5(i) == 24) /*I amominu.w */
          y = (x - b) > x ? x : b;
        else if (rv_if5(i) == 28) /*I amomaxu.w */
          y = (x - b) <= x ? x : b;
        else
          return rv_trap(cpu, RV_EILL, tval);
        if (s && (err = rv_bus(cpu, &va, (rv_u8 *)&y, 4, RV_AW)))
          return rv_trap_bus(cpu, err, va, RV_AW);
      }
      rv_sr(cpu, rv_ird(i), x);
    } else if (rv_ioph(i) == 3) {          /*Q 11/011: JAL */
      rv_sr(cpu, rv_ird(i), cpu->next_pc); /*I jal */
      cpu->next_pc = cpu->pc + rv_iimm_j(i);
    } else
      return rv_trap(cpu, RV_EILL, tval);
  } else if (rv_iopl(i) == 4) { /* ALU 部分 */
    if (rv_ioph(i) == 0 ||      /*Q 00/100: OP-IMM */
        rv_ioph(i) == 1) {      /*Q 01/100: OP */
      rv_u32 a = rv_lr(cpu, rv_irs1(i)),
             b = rv_ioph(i) ? rv_lr(cpu, rv_irs2(i)) : rv_iimm_i(i),
             s /* 备用 ALU 操作 */ = (rv_ioph(i) || rv_if3(i)) ? rv_b(i, 30) : 0,
             y /* 结果 */, sh /* 移位量 */ = b & 0x1F;
      if (!rv_ioph(i) || !rv_b(i, 25)) {
        if (rv_if3(i) == 0)      /*I add, addi, sub */
          y = s ? a - b : a + b; /* 若为备用操作则相减，否则相加 */
        else if ((rv_if3(i) == 5 || rv_if3(i) == 1) && b >> 5 & 0x5F &&
                 !rv_ioph(i))
          return rv_trap(cpu, RV_EILL, tval); /* 移位量过大！ */
        else if (rv_if3(i) == 1)              /*I sll, slli */
          y = a << sh;
        else if (rv_if3(i) == 2) /*I slt, slti */
          y = rv_ovf(a, b, a - b) != rv_sgn(a - b);
        else if (rv_if3(i) == 3) /*I sltu, sltiu */
          y = (a - b) > a;
        else if (rv_if3(i) == 4) /*I xor, xori */
          y = a ^ b;
        else if (rv_if3(i) == 5) /*I srl, srli, sra, srai */
          y = a >> (sh & 31) | (0U - (s && rv_sgn(a))) << (31 - (sh & 31));
        else if (rv_if3(i) == 6) /*I or, ori */
          y = a | b;
        else /*I and, andi */
          y = a & b;
      } else {
        return rv_trap(cpu, RV_EILL, tval);
      }
      rv_sr(cpu, rv_ird(i), y);   /* 将 ALU 输出写入寄存器 */
    } else if (rv_ioph(i) == 3) { /*Q 11/100: SYSTEM */
      rv_u32 csr /* CSR 编号 */ = rv_iimm_iu(i), y /* 结果 */;
      rv_u32 s /* uimm */ = rv_if3(i) & 4 ? rv_irs1(i) : rv_lr(cpu, rv_irs1(i));
      if ((rv_if3(i) & 3) == 1) {          /*I csrrw, csrrwi */
        if (rv_irs1(i)) {                  /* 执行 CSR 加载 */
          if (rv_csr_bus(cpu, csr, 0, &y)) /* 把 CSR 加载到 y */
            return rv_trap(cpu, RV_EILL, tval);
          if (rv_ird(i))
            rv_sr(cpu, rv_ird(i), y); /* 把 y 写入 rd */
        }
        if (rv_csr_bus(cpu, csr, 1, &s)) /* 把 CSR 置为 s */
          return rv_trap(cpu, RV_EILL, tval);
      } else if ((rv_if3(i) & 3) == 2) { /*I csrrs, csrrsi */
        if (rv_csr_bus(cpu, csr, 0, &y)) /* 把 CSR 加载到 y */
          return rv_trap(cpu, RV_EILL, tval);
        rv_sr(cpu, rv_ird(i), y), y |= s;              /* 把 y 写入 rd  */
        if (rv_irs1(i) && rv_csr_bus(cpu, csr, 1, &y)) /*  把 s|y 写入 CSR */
          return rv_trap(cpu, RV_EILL, tval);
      } else if ((rv_if3(i) & 3) == 3) { /*I csrrc, csrrci */
        if (rv_csr_bus(cpu, csr, 0, &y)) /* 把 CSR 加载到 y */
          return rv_trap(cpu, RV_EILL, tval);
        rv_sr(cpu, rv_ird(i), y), y &= ~s;             /* 把 y 写入 rd  */
        if (rv_irs1(i) && rv_csr_bus(cpu, csr, 1, &y)) /*  把 ~s&y 写入 CSR */
          return rv_trap(cpu, RV_EILL, tval);
      } else if (!rv_if3(i)) {
        if (!rv_ird(i)) {
          if (!rv_irs1(i) && rv_irs2(i) == 2 &&
              (rv_if7(i) == 8 || rv_if7(i) == 24)) { /*I mret, sret */
            rv_u32 xp /* 指令特权级 */ = rv_if7(i) >> 3;
            rv_u32 yp /* 先前（即将切入）的特权级 [mpp 或 spp] */ =
                cpu->csr.mstatus >> (xp == RV_PMACH ? 11 : 8) & xp;
            rv_u32 xpie /* 先前的 ie 位 */ = rv_b(cpu->csr.mstatus, 4 + xp);
            rv_u32 mprv /* 修改特权级 */ = rv_b(cpu->csr.mstatus, 17);
            if (rv_b(cpu->csr.mstatus, 22) && xp == RV_PSUPER)
              return rv_trap(cpu, RV_EILL, tval); /* 若 tsr=1 则产生异常 */
            mprv *= yp == RV_PMACH;               /* 若 y != m，则 mprv' = 0 */
            cpu->csr.mstatus &=
                xp == RV_PMACH ? 0xFFFDE777  /* {mpp, mie, mpie, mprv} <- 0 */
                               : 0xFFFDFEDD; /* {spp, sie, spie, mprv} <- 0 */
            cpu->csr.mstatus |= xpie << xp   /* xie <- xpie */
                                | 1 << (4 + xp) /* xpie <- 1 */
                                | mprv << 17;   /* mprv <- mprv' */
            cpu->priv = yp;                     /* priv <- y */
            cpu->next_pc = xp == RV_PMACH ? cpu->csr.mepc : cpu->csr.sepc;
          } else if (rv_irs2(i) == 5 && rv_if7(i) == 8) { /*I wfi */
            cpu->pc = cpu->next_pc;
            return (err = rv_service(cpu)) == RV_TRAP_NONE ? RV_TRAP_WFI : err;
          } else if (rv_if7(i) == 9) { /*I sfence.vma */
            if (cpu->priv == RV_PSUPER && (cpu->csr.mstatus & (1 << 20)))
              return rv_trap(cpu, RV_EILL, tval);
            cpu->tlb_valid = 0;
          } else if (!rv_irs1(i) && !rv_irs2(i) && !rv_if7(i)) { /*I ecall */
            return rv_trap(cpu, RV_EUECALL + cpu->priv, cpu->pc);
          } else if (!rv_irs1(i) && rv_irs2(i) == 1 && !rv_if7(i)) {
            return rv_trap(cpu, RV_EBP, cpu->pc); /*I ebreak */
          } else
            return rv_trap(cpu, RV_EILL, tval);
        } else
          return rv_trap(cpu, RV_EILL, tval);
      } else
        return rv_trap(cpu, RV_EILL, tval);
    } else
      return rv_trap(cpu, RV_EILL, tval);
  } else if (rv_iopl(i) == 5) {
    if (rv_ioph(i) == 0) {                           /*Q 00/101: AUIPC */
      rv_sr(cpu, rv_ird(i), rv_iimm_u(i) + cpu->pc); /*I auipc */
    } else if (rv_ioph(i) == 1) {                    /*Q 01/101: LUI */
      rv_sr(cpu, rv_ird(i), rv_iimm_u(i));           /*I lui */
    } else
      return rv_trap(cpu, RV_EILL, tval);
  } else
    return rv_trap(cpu, RV_EILL, tval);
  cpu->pc = cpu->next_pc;
  if (cpu->csr.mip && (err = rv_service(cpu)) != RV_TRAP_NONE)
    return err;
  return RV_TRAP_NONE; /* 保留编码 —— 无异常 */
}

void rv_irq(rv *cpu, rv_cause cause) {
  cpu->csr.mip &= ~(rv_u32)(RV_CSI | RV_CTI | RV_CEI);
  cpu->csr.mip |= cause;
}
