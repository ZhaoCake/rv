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
                  | rv_ext('C') /* 压缩指令 */
                  | rv_ext('M') /* 乘除法 */
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

#define rvm_lo(w) ((w) & (rv_u32)0xFFFFU) /* 32 位字的低 16 位 */
#define rvm_hi(w) ((w) >> 16)             /* 32 位字的高 16 位 */

/* 16 位加法 */
static rv_u32 rvm_ahh(rv_u32 a, rv_u32 b, rv_u32 cin, rv_u32 *cout) {
  rv_u32 sum = a + b + cin /* cin 必须小于 2。 */;
  *cout = rvm_hi(sum);
  return rvm_lo(sum);
}

/* 16 位乘法 */
static rv_u32 rvm_mhh(rv_u32 a, rv_u32 b, rv_u32 *cout) {
  rv_u32 prod = a * b;
  *cout = rvm_hi(prod);
  return rvm_lo(prod);
}

/* 32 x 32 -> 64 位乘法 */
static rv_u32 rvm(rv_u32 a, rv_u32 b, rv_u32 *hi) {
  rv_u32 al = rvm_lo(a), ah = rvm_hi(a), bl = rvm_lo(b), bh = rvm_hi(b);
  rv_u32 qh, ql = rvm_mhh(al, bl, &qh);    /* qh, ql = al * bl      */
  rv_u32 rh, rl = rvm_mhh(al, bh, &rh);    /* rh, rl = al * bh      */
  rv_u32 sh, sl = rvm_mhh(ah, bl, &sh);    /* sh, sl = ah * bl      */
  rv_u32 th, tl = rvm_mhh(ah, bh, &th);    /* th, tl = ah * bh      */
  rv_u32 mc, m = rvm_ahh(rl, sl, 0, &mc);  /*  m, nc = rl + sl      */
  rv_u32 nc, n = rvm_ahh(rh, sh, mc, &nc); /*  n, nc = rh + sh + nc */
  rv_u32 x = ql;                           /*  x, 0  = ql           */
  rv_u32 yc, y = rvm_ahh(m, qh, 0, &yc);   /*  y, yc = qh + m       */
  rv_u32 zc, z = rvm_ahh(n, tl, yc, &zc);  /*  z, zc = tl + n  + yc */
  rv_u32 wc, w = rvm_ahh(th, nc, zc, &wc); /*  w, 0  = th + nc + zc */
  *hi = z | (w << 16);                     /*   hi   = (w, z)       */
  return x | (y << 16);                    /*   lo   = (y, x)       */
}

#define rvc_op(c) rv_bf(c, 1, 0)           /* c. 操作码 */
#define rvc_f3(c) rv_bf(c, 15, 13)         /* c. funct3 */
#define rvc_rp(r) ((r) + 8)                /* c. r' 寄存器偏移量 */
#define rvc_ird(c) rv_bf(c, 11, 7)         /* c. ci 格式 rd/rs1  */
#define rvc_irpl(c) rvc_rp(rv_bf(c, 4, 2)) /* c. rd'/rs2'（位 4-2） */
#define rvc_irph(c) rvc_rp(rv_bf(c, 9, 7)) /* c. rd'/rs1'（位 9-7） */
#define rvc_imm_ciw(c)                     /* c.addi4spn 的 CIW 立即数 */        \
  (rv_tbf(c, 10, 7, 6) | rv_tbf(c, 12, 11, 4) | rv_tb(c, 6, 2) | rv_tb(c, 5, 3))
#define rvc_imm_cl(c) /* c.lw/c.sw 的 CL 立即数 */                              \
  (rv_tb(c, 5, 6) | rv_tbf(c, 12, 10, 3) | rv_tb(c, 6, 2))
#define rvc_imm_ci(c) /* c.addi/c.li/c.lui 的 CI 立即数 */                      \
  (rv_signext(rv_tb(c, 12, 5), 5) | rv_bf(c, 6, 2))
#define rvc_imm_ci_b(c) /* c.addi16sp 的 CI 立即数 */                           \
  (rv_signext(rv_tb(c, 12, 9), 9) | rv_tbf(c, 4, 3, 7) | rv_tb(c, 5, 6) |      \
   rv_tb(c, 2, 5) | rv_tb(c, 6, 4))
#define rvc_imm_ci_c(c) /* c.lwsp 的 CI 立即数 */                               \
  (rv_tbf(c, 3, 2, 6) | rv_tb(c, 12, 5) | rv_tbf(c, 6, 4, 2))
#define rvc_imm_cj(c) /* c.jalr/c.j 的 CJ 立即数 */                             \
  (rv_signext(rv_tb(c, 12, 11), 11) | rv_tb(c, 11, 4) | rv_tbf(c, 10, 9, 8) |  \
   rv_tb(c, 8, 10) | rv_tb(c, 7, 6) | rv_tb(c, 6, 7) | rv_tbf(c, 5, 3, 1) |    \
   rv_tb(c, 2, 5))
#define rvc_imm_cb(c) /* c.beqz/c.bnez 的 CB 立即数 */                          \
  (rv_signext(rv_tb(c, 12, 8), 8) | rv_tbf(c, 6, 5, 6) | rv_tb(c, 2, 5) |      \
   rv_tbf(c, 11, 10, 3) | rv_tbf(c, 4, 3, 1))
#define rvc_imm_css(c) /* c.swsp 的 CSS 立即数 */                               \
  (rv_tbf(c, 8, 7, 6) | rv_tbf(c, 12, 9, 2))

/* 组装所有非压缩指令类型的宏 */
#define rv_i_i(op, f3, rd, rs1, imm) /* I 型 */                                \
  ((imm) << 20 | (rs1) << 15 | (f3) << 12 | (rd) << 7 | (op) << 2 | 3)
#define rv_i_s(op, f3, rs1, rs2, imm) /* S 型 */                               \
  (rv_bf(imm, 11, 5) << 25 | (rs2) << 20 | (rs1) << 15 | (f3) << 12 |          \
   rv_bf(imm, 4, 0) << 7 | (op) << 2 | 3)
#define rv_i_u(op, rd, imm) /* U 型 */                                         \
  ((imm) << 12 | (rd) << 7 | (op) << 2 | 3)
#define rv_i_r(op, f3, rd, rs1, rs2, f7) /* R 型 */                            \
  ((f7) << 25 | (rs2) << 20 | (rs1) << 15 | (f3) << 12 | (rd) << 7 |           \
   (op) << 2 | 3)
#define rv_i_j(op, rd, imm) /* J 型 */                                         \
  (rv_b(imm, 20) << 31 | rv_bf(imm, 10, 1) << 21 | rv_b(imm, 11) << 20 |       \
   rv_bf(imm, 19, 12) << 12 | (rd) << 7 | (op) << 2 | 3)
#define rv_i_b(op, f3, rs1, rs2, imm) /* B 型 */                               \
  (rv_b(imm, 12) << 31 | rv_bf(imm, 10, 5) << 25 | (rs2) << 20 | (rs1) << 15 | \
   (f3) << 12 | rv_bf(imm, 4, 1) << 8 | rv_b(imm, 11) << 7 | (op) << 2 | 3)

/* 解压指令 */
static rv_u32 rvc(rv_u32 c) {
  if (rvc_op(c) == 0) {
    if (rvc_f3(c) == 0 && c != 0) { /* c.addi4spn -> addi rd', x2, nzuimm */
      return rv_i_i(4, 0, rvc_irpl(c), 2, rvc_imm_ciw(c));
    } else if (c == 0) { /* 非法指令 */
      return 0;
    } else if (rvc_f3(c) == 2) { /*I c.lw -> lw rd', offset(rs1') */
      return rv_i_i(0, 2, rvc_irpl(c), rvc_irph(c), rvc_imm_cl(c));
    } else if (rvc_f3(c) == 6) { /*I c.sw -> sw rs2', offset(rs1') */
      return rv_i_s(8, 2, rvc_irph(c), rvc_irpl(c), rvc_imm_cl(c));
    } else { /* 非法指令 */
      return 0;
    }
  } else if (rvc_op(c) == 1) {
    if (rvc_f3(c) == 0) { /*I c.addi -> addi rd, rd, nzimm */
      return rv_i_i(4, 0, rvc_ird(c), rvc_ird(c), rvc_imm_ci(c));
    } else if (rvc_f3(c) == 1) { /*I c.jal -> jal x1, offset */
      return rv_i_j(27, 1, rvc_imm_cj(c));
    } else if (rvc_f3(c) == 2) { /*I c.li -> addi rd, x0, imm */
      return rv_i_i(4, 0, rvc_ird(c), 0, rvc_imm_ci(c));
    } else if (rvc_f3(c) == 3) { /* 01/011: LUI/ADDI16SP */
      if (rvc_ird(c) == 2) {     /*I c.addi16sp -> addi x2, x2, nzimm */
        return rv_i_i(4, 0, 2, 2, rvc_imm_ci_b(c));
      } else if (rvc_ird(c) != 0) { /*I c.lui -> lui rd, nzimm */
        return rv_i_u(13, rvc_ird(c), rvc_imm_ci(c));
      } else { /* 非法指令 */
        return 0;
      }
    } else if (rvc_f3(c) == 4) {   /* 01/100: MISC-ALU */
      if (rv_bf(c, 11, 10) == 0) { /*I c.srli -> srli rd', rd', shamt */
        return rv_i_r(4, 5, rvc_irph(c), rvc_irph(c), rvc_imm_ci(c) & 0x1F, 0);
      } else if (rv_bf(c, 11, 10) == 1) { /*I c.srai -> srai rd', rd', shamt */
        return rv_i_r(4, 5, rvc_irph(c), rvc_irph(c), rvc_imm_ci(c) & 0x1F, 32);
      } else if (rv_bf(c, 11, 10) == 2) { /*I c.andi -> andi rd', rd', imm */
        return rv_i_i(4, 7, rvc_irph(c), rvc_irph(c), rvc_imm_ci(c));
      } else if (rv_bf(c, 11, 10) == 3) {
        if (rv_bf(c, 6, 5) == 0) { /*I c.sub -> sub rd', rd', rs2' */
          return rv_i_r(12, 0, rvc_irph(c), rvc_irph(c), rvc_irpl(c), 32);
        } else if (rv_bf(c, 6, 5) == 1) { /*I c.xor -> xor rd', rd', rs2' */
          return rv_i_r(12, 4, rvc_irph(c), rvc_irph(c), rvc_irpl(c), 0);
        } else if (rv_bf(c, 6, 5) == 2) { /*I c.or -> or rd', rd', rs2' */
          return rv_i_r(12, 6, rvc_irph(c), rvc_irph(c), rvc_irpl(c), 0);
        } else if (rv_bf(c, 6, 5) == 3) { /*I c.and -> and rd', rd', rs2' */
          return rv_i_r(12, 7, rvc_irph(c), rvc_irph(c), rvc_irpl(c), 0);
        } else { /* 非法指令 */
          return 0;
        }
      } else { /* 非法指令 */
        return 0;
      }
    } else if (rvc_f3(c) == 5) { /*I c.j -> jal x0, offset */
      return rv_i_j(27, 0, rvc_imm_cj(c));
    } else if (rvc_f3(c) == 6) { /*I c.beqz -> beq rs1' x0, offset */
      return rv_i_b(24, 0, rvc_irph(c), 0, rvc_imm_cb(c));
    } else if (rvc_f3(c) == 7) { /*I c.bnez -> bne rs1' x0, offset */
      return rv_i_b(24, 1, rvc_irph(c), 0, rvc_imm_cb(c));
    } else { /* 非法指令 */
      return 0;
    }
  } else if (rvc_op(c) == 2) {
    if (rvc_f3(c) == 0) { /*I c.slli -> slli rd, rd, shamt */
      return rv_i_r(4, 1, rvc_ird(c), rvc_ird(c), rvc_imm_ci(c) & 0x1F, 0);
    } else if (rvc_f3(c) == 2) { /*I c.lwsp -> lw rd, offset(x2) */
      return rv_i_i(0, 2, rvc_ird(c), 2, rvc_imm_ci_c(c));
    } else if (rvc_f3(c) == 4 && !rv_b(c, 12) && !rv_bf(c, 6, 2)) {
      /*I c.jr -> jalr x0, 0(rs1) */
      return rv_i_i(25, 0, 0, rvc_ird(c), 0);
    } else if (rvc_f3(c) == 4 && !rv_b(c, 12)) { /*I c.mv -> add rd, x0, rs2 */
      return rv_i_r(12, 0, rvc_ird(c), 0, rv_bf(c, 6, 2), 0);
    } else if (rvc_f3(c) == 4 && rv_b(c, 12) && rvc_ird(c) &&
               !rv_bf(c, 6, 2)) { /*I c.jalr -> jalr x1, 0(rs1) */
      return rv_i_i(25, 0, 1, rvc_ird(c), 0);
    } else if (rvc_f3(c) == 4 && rv_b(c, 12) && !rvc_ird(c) &&
               !rv_bf(c, 6, 2)) { /*I c.ebreak -> ebreak */
      return rv_i_i(28, 0, 0, 0, 1);
    } else if (rvc_f3(c) == 4 && rv_b(c, 12) && rvc_ird(c) &&
               rv_bf(c, 6, 2)) { /*I c.add -> add rd, rd, rs2 */
      return rv_i_r(12, 0, rvc_ird(c), rvc_ird(c), rv_bf(c, 6, 2), 0);
    } else if (rvc_f3(c) == 6) { /*I c.swsp -> sw rs2, offset(x2) */
      return rv_i_s(8, 2, 2, rv_bf(c, 6, 2), rvc_imm_css(c));
    } else { /* 非法指令 */
      return 0;
    }
  } else { /* 非法指令 */
    return 0;
  }
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
 *      rv_bus    ← 对齐检查、地址翻译、跨页拆分、字节序转换
 *        │  交给宿主的已经是"物理地址"
 *        ▼
 *      bus_cb    ← 宿主实现的地址解码器（RAM / UART / PLIC / CLINT ...）
 *
 * 参数：
 *   va     [进/出] 虚拟地址。跨页时会被就地前移，所以必须传指针。
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
   * 这一步还有个副作用：它保证地址按 width 对齐，而 4096 能被 1、2、4 整除，
   * 因此首尾字节必然落在同一页内 —— 这正是下面"跨页分支"永远不成立的原因。 */
  if (*va & (width - 1))
    return RV_BAD_ALIGN;

  /* 步骤 3：虚拟地址 -> 物理地址。
   * rv_vmm 内部在 TLB 未命中时会去读页表，那条路径走的是 cpu->bus_cb
   * 而不是本函数（理由见函数头第 1 条约定）。
   * 失败时 err 可能是 RV_BAD（访问异常）或 RV_PAGEFAULT（缺页）。 */
  if ((err = rv_vmm(cpu, *va, &pa, access)))
    return err; /* 缺页或访问异常 */

  /* 步骤 4：跨页拆分。
   * 判断方法：((pa + width - 1) ^ pa) 取的是"首地址与末地址不同的那些位"，
   * 再 & ~0xFFF 看这些差异是否落在页号（高 20 位）上；若是，说明这次访问
   * 跨越了 4KB 边界，需要拆成两段分别访问。
   *
   * 注意（重要）：由于步骤 2 已保证地址按 width 对齐、而 4096 是 width 的倍数，
   * 首字节与末字节必然同页 —— 这个分支实际上永远不会进入，属于预留代码。
   *
   * 而且它即便被进入也是错的：下面只对 width / va / data 做了前移，偏偏没有
   * 对 ledata 偏移，也没有在段间把数据落回 data。后果是
   *   load ：第一段读进 ledata 后无处落脚，被第二段覆盖 -> 页 A 的字节丢失；
   *   store：第二段的源又从头取一遍 -> 页 B 被写成页 A 的字节。
   * 以 lw 0x80000FFE 为例（页 A 尾部 = {11,22}、页 B 头部 = {33,34}）：
   * 正确结果应为 34332211，按当前写法只会得到 34330000。
   * 想让它正确，必须同时做到三件事：
   *   ① 第二段访问使用 ledata + w0（否则 store 写重复）；
   *   ② 段间不要前移 data，让拼好的数据落在缓冲开头；
   *   ③ 收尾的 rv_endcvt 使用"原始的" data 与 width（否则 load 只搬回第二段）。 */
  if (((pa + width - 1) ^ pa) & ~0xFFFU) /* 跨越页边界 */ {
    rv_u32 w0 /* 从第一页加载这么多字节 */ = 0x1000 - (*va & 0xFFF);
    /* 第一段：访问第一页剩下的 w0 个字节 */
    if ((err = cpu->bus_cb(cpu->user, pa, ledata, access == RV_AW, w0)))
      return err;
    /* 三个量一起前移到第二页开头：剩余宽度、虚拟地址、数据缓冲。
     * 注意 width 递减后，步骤 6 的 rv_endcvt 就只会写回第二段的数据。 */
    width -= w0, *va += w0, data += w0;
    /* 换页了，物理地址必须基于新虚拟地址重新翻译一次 */
    if ((err = rv_vmm(cpu, *va, &pa, RV_AW)))
      return err;
  }

  /* 步骤 5：真正的总线访问 —— 全函数唯一把请求交给宿主的地方。
   * 只有 access == RV_AW 时才置 is_store，所以取指（RV_AX）和读（RV_AR）
   * 在 bus_cb 看来都是"读"。返回非 0 时原样向上传递（见步骤 2 的说明）。 */
  if ((err = cpu->bus_cb(cpu->user, pa, ledata, access == RV_AW, width)))
    return err;

  /* 步骤 6：把 bus_cb 填好的字节序列重新组装成 rv 侧的整数，写回 data。
   * is_store 传 0，表示方向是 "宿主侧 -> rv 侧"。 */
  rv_endcvt(ledata, data, width, 0);
  return 0; /* 等价于 RV_OK */
}

/* 取指 */
static rv_u32 rv_if(rv *cpu, rv_u32 *i, rv_u32 *tval) {
  rv_u32 err, page = (cpu->pc ^ (cpu->pc + 3)) & ~0xFFFU, pc = cpu->pc;
  if (cpu->pc & 2 || page) { /* 分两次 2 字节完成取指 */
    rv_u32 ia /* 指令前半部分 */ = 0, ib /* 后半部分 */ = 0;
    if ((err = rv_bus(cpu, &pc, (rv_u8 *)&ia, 2, RV_AX))) /* 取第 1 半 */
      goto error;
    if (rv_isz(ia) == 4 && (pc += 2, 1) && /* 若指令宽 4 字节 */
        (err = rv_bus(cpu, &pc, (rv_u8 *)&ib, 2, RV_AX))) /* 取第 2 半 */
      goto error; /* 上面的 pc += 2 是为了让 {page}fault trap 的地址准确 */
    *i = (rv_u32)ia | (rv_u32)ib << 16U;
  } else if ((err = rv_bus(cpu, &pc, (rv_u8 *)i, 4, RV_AX))) /* 4 字节取指 */
    goto error;
  cpu->next_pc = cpu->pc + rv_isz(*i);
  *tval = *i; /* 对非法指令 trap 而言，tval 是原始指令 */
  if (rv_isz(*i) < 4)
    *i = rvc(*i & 0xFFFF);
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
  rv_u32 i, tval, err = rv_if(cpu, &i, &tval); /* 取指令到 i */
  if (!++cpu->csr.cycle)
    cpu->csr.cycleh++; /* 带进位地累加到 cycle、cycleh */
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
      } else if (rv_ioph(i) == 1 && rv_if7(i) == 1) {
        rv_u32 as /* sgn(a) */ = 0, bs /* sgn(b) */ = 0, ylo, yhi /* 结果 */;
        if (rv_if3(i) < 4) {              /*I mul, mulh, mulhsu, mulhu */
          if (rv_if3(i) < 3 && rv_sgn(a)) /* 当 f3 属于 {0, 1, 2} 时 a 为有符号 */
            a = ~a + 1, as = 1;           /* 二进制补码 */
          if (rv_if3(i) < 2 && rv_sgn(b)) /* 当 f3 属于 {0, 1} 时 b 为有符号 */
            b = ~b + 1, bs = 1;           /* 二进制补码 */
          ylo = rvm(a, b, &yhi);          /* 执行乘法 */
          if (as != bs) /* 若结果 < 0，则对输出值取反 */
            ylo = ~ylo + 1, yhi = ~yhi + !ylo; /* 二进制补码 */
          y = rv_if3(i) ? yhi : ylo; /* 若是 mulh 则返回高字，否则返回低字 */
        } else {
          if (rv_if3(i) == 4) /*I div */
            y = b ? (rv_u32)((rv_s32)a / (rv_s32)b) : (rv_u32)(-1);
          else if (rv_if3(i) == 5) /*I divu */
            y = b ? (a / b) : (rv_u32)(-1);
          else if (rv_if3(i) == 6) /*I rem */
            y = b ? (rv_u32)((rv_s32)a % (rv_s32)b) : a;
          else  /*I remu */
            y = b ? a % b : a;
        } /* 这一切都是因为我们没有 64 位。值得吗？大概不值 B) */
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
