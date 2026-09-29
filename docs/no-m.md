# no-m：移除 M 扩展的记录

| 项目 | 内容 |
|---|---|
| 分支 | `main`（保底锚点 `no-rvc-with-m`） |
| 日期 | 2026-09-29 |
| 目标 | 得到一个纯净的 `rv32ia_zicsr_zifencei` 核心，作为"阶段 0"在模拟器侧的准备 |
| 结果 | `rv.c` **598 行 → 542 行**；`git diff no-rvc-with-m` = `+6 / -63` |
| 状态 | 未提交；编译零警告，`example` 行为不变 |

相关文档：[`no-rvc.md`](no-rvc.md)（移除 C 扩展）、[`design-tradeoffs.md`](design-tradeoffs.md)（扩展取舍与阶段划分）

---

## 1. 动机：这次不是"为了省面积"

这一点必须先说清楚，否则容易误读这次改动的意义。

| 常见的误读 | 实际情况 |
|---|---|
| "删 M 是为了省芯片面积" | **M 里真正贵的是除法器**，乘法器相对便宜。单纯删 M 会**连乘法一起删掉**，得到一个更慢的核，却只省下乘法器的面积 |
| "删 M 等于换成 Zmmul" | **不等价**。Zmmul 是"保留乘法、只去掉除法"，需要在 `rv_step` 里**新增**一套 Zmmul 分派；而本次是**整块删除** |

所以本次改动的真实目的是：

> **把模拟器侧对齐到"阶段 0"的目标 ISA —— `rv32ia_zicsr_zifencei`**，
> 这样"无 M 的软件栈"才有东西可以验证。

换句话说，**这是为验证软件栈做的准备，不是微架构优化**。真正"用 Zmmul 省掉除法器"的路线，属于阶段 1+ 的独立工作（见第 5 节）。

---

## 2. 删掉的内容

| # | 位置（`no-rvc-with-m` 分支的行号） | 内容 | 行数 |
|---|---|---|---|
| 1 | `rv.c:16` | `misa` 里的 `\| rv_ext('M')` | 1 |
| 2 | `rv.c:200-232` | `rvm_lo` / `rvm_hi` 宏、`rvm_ahh()`（16 位加法）、`rvm_mhh()`（16 位乘法）、`rvm()`（32×32→64 位手工进位链） | 34 |
| 3 | `rv.c:492-512` | `rv_step` 里的 M 分派：`mul` / `mulh` / `mulhsu` / `mulhu` / `div` / `divu` / `rem` / `remu` | 21 |

删除后，`rv_step` 的 ALU 分支结构简化为：

```c
if (!rv_ioph(i) || !rv_b(i, 25)) {
    /* Q 00/100 OP-IMM 与 Q 01/100 OP：基础 ALU */
} else {
    return rv_trap(cpu, RV_EILL, tval);   /* funct7 != 0 的 OP 指令全部非法 */
}
```

于是 `mul` 系列（`funct7 = 1`）与未来的 Zbb（`funct7 = 0x30`）都会干净地落到"非法指令"上。

---

## 3. 配套更新（否则仓库会"说错话"）

| 文件 | 改动 |
|---|---|
| `README.md` | ISA 描述 `RV32IMAC_Zicsr` → `RV32IA_Zicsr_Zifencei`；"约 800 行" → "约 540 行"；删除"不使用任何大于 32 位的整数类型，**乘法也是如此**"中的后半句（乘法代码已不存在）；GCC 命令行 `-march=rv32imac` → `-march=rv32ia_zicsr_zifencei`；"可以启动 RISCV32 Linux"补上"需按无 C / 无 M 重新构建的镜像"的限定并指向 `no-rvc.md` |
| `tools/test/Makefile` | 移除 `rv32uc-p-*`（C 扩展）与 `rv32um-p-*`（M 扩展）测试向量的拷贝 |

**规则**：每次砍掉一个扩展，就要回头检查三处——**README 的 ISA 描述、测试向量列表、编译命令行**。它们是最容易"说谎"的地方。

---

## 4. 验证

### 4.1 `misa` 的预期值

```
misa = 0x40140001
       │││││└─ bit 0  : A
       ││││└── bit 18 : S
       │││└─── bit 20 : U
       ││└──── bit 12 : M   ← 已清零
       │└───── bit 2  : C   ← 在 no-rvc 阶段已清零
       └────── bit 30 : MXL = 1（XLEN = 32）
```

**顺带记住 misa 的位分配规律**：它就是"按字母顺序"排的（A=0, B=1, C=2, D=3, …, M=12, …, S=18, …, U=20）。
`rv.c` 里的宏正是这条规律的直译：

```c
#define rv_ext(c) (1 << (rv_u8)((c) - 'A')) /* misa 中的 ISA 扩展位 */
```

所以 `rv_ext('M') = 1 << 12`、`rv_ext('C') = 1 << 2`。

### 4.2 探针：让 `mul` 撞上非法指令

程序第一条是 `mul a7, a7, a0`（编码 `0x02A888B3`，`funct7 = 1`）：

```c
rv_u32 program[2] = {
    0x02A888B3, /* mul a7, a7, a0 */
    0x00000073, /* ecall */
};
```

| 分支 | `misa` | `rv_step` 返回 | `mtval` | 说明 |
|---|---|---|---|---|
| `no-rvc-with-m` | `0x40141001` | `0x80000010`（RV_TRAP_NONE） | `0` | 乘法正常执行 |
| `main`（本次） | `0x40140001` | `2`（RV_EILL） | `0x02A888B3` | 被判非法指令，`mtval` 为指令编码 |

回归验证：`example.c`（纯 I 型指令）在两次改动后行为完全一致，始终输出 `Environment call @ 80000004: 42`。

---

## 5. 没有做的事（以及为什么）

| 没做 | 原因 |
|---|---|
| 删 `rv.h` 里的 `rv_s32` | 它是公开头文件的类型定义，只被 M 的 `div`/`rem` 使用。删它要连带改 3 处 `RV_S32_TYPE` 宏（三个 `#if` 分支），收益 1 行、牵连 3 处，不值 |
| 把 M 换成 Zmmul | Zmmul 需要**新增**分派（保留 `mul`/`mulh`/`mulhu`/`mulhsu`，去掉 `div`/`rem`），是阶段 1+ 的独立工作，不属于"删 M" |
| 实现 Zbb | 同上，属于阶段 1+ |
| 删除 `rv_ovf` / `rv_sgn` | 它们不只服务 M：`rv_ovf` 还用于分支比较、`slt`、`amomin`/`amomax`，必须保留 |

---

## 6. 复现验证

```powershell
# 回归（应与改动前完全一致）
gcc --std=c89 -Wall -Wextra -pedantic -Wshadow -g -I. rv.c tools/example/example.c -o example.exe
./example.exe
# 期望：Environment call @ 80000004: 42

# 探针：mul 应被拒绝
gcc --std=c89 -I. rv.c probe_nom.c -o probe.exe && ./probe.exe
# 期望：misa = 40140001；rv_step = 2 (RV_EILL)；mtval = 02A888B3

# 对照（有 M 的版本）
git show no-rvc-with-m:rv.c > rv_withm.c
gcc --std=c89 -I. rv_withm.c probe_nom.c -o probe2.exe && ./probe2.exe
# 期望：misa = 40141001；rv_step = 0x80000010 (TRAP_NONE)
```

---

## 7. 对下一步的意义

至此模拟器侧已经**完全对齐"阶段 0"的目标配置**：

```
I ✓   A ✓   Zicsr ✓   Zifencei ✓   S 模式 ✓   Sv32 ✓   （C 已移除、M 已移除）
```

于是剩下的工作量全在**软件栈**（对应 [`design-tradeoffs.md`](design-tradeoffs.md) 阶段 0 的四个改动点）：

1. 内核 `arch/riscv/Makefile` 的 march 串去掉 `m`
2. 提供 `__mulsi3` / `__divsi3` / `__udivsi3` / `__modsi3` / `__umodsi3`
3. OpenSBI 的 `PLATFORM_RISCV_ISA` 同步改
4. 工具链 multilib 匹配（见 [`no-rvc-toolchain.md`](no-rvc-toolchain.md)）

**模拟器这边已经没有阻塞项了。** 如果后续要走 Zmmul / Zbb 路线，那就是在阶段 0 跑通之后、按实测瓶颈再决定的事。
