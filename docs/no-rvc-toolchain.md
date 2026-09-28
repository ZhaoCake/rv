# 无 C 的软件栈：为什么"选项选对了"还是带 C

> 记录一次讨论：把 OpenSBI、Linux 内核、buildroot 的 C 扩展选项全部关掉，
> 最终跑起来的软件栈里**仍然**带着压缩指令。问题出在工具链，不在 CPU，也不在源码。

## 结论先行

**C 扩展不是跑 Linux 的必要条件。** 但要让整个栈真正无 C，只关构建开关远远不够——**工具链提供的库（libc / libgcc）必须也是无 C 编译的**，否则用户空间这一环会把 C 指令偷偷带回来，而且失败点被推迟到运行期，非常难定位。

一句话：**这不是 ISA 问题，是构建系统与 ABI 分组方式的问题。**

---

## 1. 先把"必需"和"可选"分清楚

| 特性 | 必要性 | 理由 |
|---|---|---|
| RV32I / RV64I 基础指令 | **必需** | 指令集根本 |
| Zicsr | **必需** | `mstatus` / `satp` / `mepc` / `mcause` / `stvec` 全靠它 |
| Zifencei（`fence.i`） | **必需** | 指令缓存同步；模块加载、`flush_icache` 依赖它 |
| A（原子） | **必需** | 自旋锁、原子计数器；没有它内核无法工作 |
| S 模式 | **必需** | Linux 内核跑在 S 模式，M 模式归固件 |
| Sv32 / Sv39 MMU | **必需** | 虚拟内存是内核的地基 |
| M（乘除） | 实践上必需 | 规范上可用 libgcc 软件模拟，但性能是灾难 |
| **C（压缩）** | **可选** | 纯粹的代码体积 / 取指效率优化 |

所以"没有 C 就跑不了 Linux"是**错误**的说法。正确的说法是："C 扩展是生态默认打开的性能优化。"

---

## 2. 三处构建期开关（全都能关）

| 层 | 开关 | 说明 |
|---|---|---|
| Linux 内核 | `CONFIG_RISCV_ISA_C` | bool；关掉后内核按 `-march=rv32ima` 之类编译 |
| OpenSBI | `PLATFORM_RISCV_ISA` | 构建变量，可直接写 `rv32ima` |
| buildroot | `BR2_RISCV_ISA_RVC` | 决定用户空间如何编译 |

历史事实：RISC-V 的 Linux 支持最早走的就是 RV32IMA / RV64IMA，"带 C"是后来为了省代码体积才普遍启用的。

---

## 3. 编译器这一层没有问题

GCC 只在 `-march` 字符串**包含 `c`** 时才生成压缩指令：

```
-march=rv32imac  → 会生成 c.* 指令
-march=rv32ima   → 不会生成 c.* 指令
```

也就是说，**源码和编译器都不构成障碍**。障碍在下一步。

---

## 4. 真正的坑：ABI 相同，而 multilib 不区分"C 与否"

关键事实：

- `rv32ima` 与 `rv32imac` 使用**完全相同的 ABI**：`ilp32`（都是 32 位整数、软浮点）。
- GCC 的 multilib 目录是按 **march / mabi 组合**分组的，而标准工具链在构建时通常只生成"带 C"的那一套。
- 于是当你用 `-march=rv32ima` 编译并链接时，需要一套 `ilp32`（无 C）的 libc / libgcc / crt 文件——**如果工具链没准备，就会出现两种结果**：

| 工具链配置 | 表现 |
|---|---|
| 严格匹配 multilib | 链接期直接报错：找不到匹配的 multilib 或库 |
| 宽松 / ABI 兼容回退 | 静默用上了按 `rv32imac` 编的库 → **运行期**才炸 |

第二种更糟：**内核能正常启动，用户空间 init 一跑就抛 illegal instruction**，而这时的报错信息完全指不到"库里有 C 指令"这个根因。

这正是"明明选项是对的，却还是带 C"的来源——**你关掉的是"用什么编译"的选项，但没关掉"链接了谁"**。

---

## 5. 典型失败链条

```
① 关掉 CONFIG_RISCV_ISA_C          → 内核镜像无 C     ✓
② 关掉 PLATFORM_RISCV_ISA 里的 c   → OpenSBI 无 C     ✓
③ 关掉 BR2_RISCV_ISA_RVC           → busybox 等应用无 C ✓
④ 但链接的是工具链预编译的 ilp32 libc（实际按 rv32imac 编）  ✗ ← 问题在这里
⑤ 运行到该库里的某条 c.*           → illegal instruction
```

注意第 ④ 步：**它不在你关掉的任何一个选项的管辖范围内。**

---

## 6. 怎么验证一个二进制里到底有没有 C

**方法 A：查 ELF 属性段（最快、最可靠）**

```shell
readelf -A vmlinux
# 输出中会有一行 Tag_RISCV_arch: rv32i2p0_m2p0_a2p0_c2p0 ...
#                                                  ^^^^ 有 c 就是带了压缩扩展
```

**方法 B：反汇编里找压缩指令**

```shell
riscv64-unknown-elf-objdump -d -M no-aliases binary | grep -E '^\s+[0-9a-f]+:\s+[0-9a-f]{4}\s'
```

看反汇编的**每条指令长度**：如果出现 2 字节（4 个十六进制字符）的指令，那就是压缩指令。加 `-M no-aliases` 可以避免别名干扰判断。

**建议**：对内核、OpenSBI、libc、busybox 分别做一次检查，就能定位到 C 指令是从哪一环混进来的。

---

## 7. 要真正落地"无 C 的完整栈"，需要做什么

| 方案 | 做法 | 说明 |
|---|---|---|
| **A. 自建工具链** | riscv-gnu-toolchain 用 `--with-multilib-generator` 增加 `rv32ima/ilp32` 这一组 | 最正统；一次性成本高 |
| **B. 用 buildroot 内部工具链** | 关掉 `BR2_RISCV_ISA_RVC` 让 buildroot 连 libc 一起编 | 最省事，理论上可一次到位（glibc/musl 会跟着用无 C 编译） |
| **C. 用外部预编译工具链 + 自编 libc** | 应用静态链接自编的 libc | 适合裁剪场景 |
| **D. 接受带 C** | 现状 | 现实中几乎所有发行版都选这条 |

**方案 B 是最值得先试的**：因为 buildroot 内部工具链会把 libc 也编译一遍，而 `BR2_RISCV_ISA_RVC` 同时作用于工具链和用户空间。

---

## 8. 待验证的开放问题

1. 方案 B 在 buildroot 里实际是否可行？需要确认 `BR2_RISCV_ISA_RVC=n` 是否真的传导到了工具链的 `--with-arch`，以及 glibc / musl 是否有强制使用 C 指令的地方。
2. 标准 `riscv-gnu-toolchain` 默认构建的 multilib 集合里，是否存在无 C 的那一组？（`gcc -print-multi-lib` 可以直接列出来，是最快的检查手段。）
3. 用本仓库 `no-rvc` 分支（`rv.c` 已移除 C 支持）配合一套无 C 的软件栈，能否真的启动到 shell？这是最终验收。

> 检查命令备忘：
> ```shell
> riscv64-unknown-elf-gcc -print-multi-lib     # 列出工具链实际提供的 multilib
> riscv64-unknown-elf-gcc -print-multi-directory -march=rv32ima -mabi=ilp32
>                                              # 看它会去哪找库（空 = 用的是默认库）
> ```

---

## 9. 相关文档

- [`no-rvc.md`](no-rvc.md) —— 从模拟器侧移除 C 扩展的完整魔改记录（五刀、`rv.c` 761 → 598 行）
- [`TASK.md`](TASK.md) —— 学习清单；其中 todoN 涉及 buildroot 配置与 ISA 必要性
