# TASK — 吃透「一个能跑 Linux 的 RV 平台到底需要什么」

> 目标不是看懂 `rv.c` 这 800 行，而是建立一张完整的因果图：从模拟器进程启动到 Linux 打出第一行日志，
> 每一步依赖哪个硬件 / 机制，以及本仓库把它实现在哪一行。
>
> 推进方式：严格按 todoA → todoN 顺序，前一项的「通过标准」没过不要往下走。
> 每项包含四项：**目标**（要建立的认知）、**代码坐标**（去哪读）、**突破点**（最容易卡住的地方）、**通过标准**（可验收，必须动脑或动手）。

---

## todoA 四层结构：搞清楚谁负责什么

- **目标**：建立「CPU 核 / 平台外设 / M 模式固件 / S 模式内核」四层分工模型，以及仓库文件地图。
- **代码坐标**：`README.md`、`tools/linux/README.md`、`tools/linux/mach.c`、`rv.c`、`tools/linux/extern/rv.dts`。
- **突破点**：Linux 内核并不直接操作机器，它跑在 S 模式；M 模式被 OpenSBI 独占。`mach.c` 模拟的是「主板」，`rv.c` 模拟的是「芯片里的 CPU 核」，`rv.dts` 是把主板信息告诉 Linux 的说明书。
- **通过标准**：能不看代码画出四层框图，并说出每层对应哪些文件；能回答「`rv.c` 里为什么找不到任何 UART / 定时器代码」。

## todoB 译码骨架：一条指令如何从内存走到执行

- **目标**：掌握 `rv_step` 的主分派结构，理解 RISC-V 编码正交性带来的「查表式」实现。
- **代码坐标**：`rv.c:447`（`rv_step`）、`rv.c` 行 39-59（`rv_iopl` / `rv_ioph` / `rv_if3` / `rv_iimm_*` 字段宏）、`rv.c:281`（`rvc` 解压）、`rv.c` 行 263-278（各类型编码组装宏）。
- **突破点**：压缩指令不是单独一条执行路径，而是 `rv_if` 先解压成等价 32 位指令再走同一套译码；`/*Q xx/xxx` 注释就是 RISC-V 规范的「象限图」坐标。
- **通过标准**：能任取 `rv.c` 中一条指令臂（如 `beq`），说明它由哪几个字段宏算出，以及为什么 `rv_iopl(i) == 0 && rv_ioph(i) == 3` 就是分支指令。

## todoC trap 与特权级：控制权的交接仪式

- **目标**：搞清异常 / 中断发生时的完整状态迁移，以及 M→S 的委派机制。
- **代码坐标**：`rv.c:119`（`rv_trap`）、`rv.c:147`（`rv_trap_bus`）、`rv.c:434`（`rv_service`）、`rv.c:631`（`mret` / `sret`）。
- **突破点**：`rv_trap` 用一个表达式同时决定目标特权级（查 `medeleg`/`mideleg`）、被改写的寄存器组（m* 还是 s*）、以及 `mstatus` 的 `{xpp, xie, xpie}` 压栈；`mtvec` 最低位决定向量模式。
- **通过标准**：能手工推演「S 模式下执行 `ecall`」时 `mepc/mcause/mstatus/priv/pc` 各变成什么；能解释 OpenSBI 为什么要把大部分异常委派给 S 模式。

## todoD CSR 访问：为什么它被建模成一条总线

- **目标**：看透 `rv_csr_bus` 的表驱动设计——同一条总线上有多个「地址」映射到同一个物理寄存器。
- **代码坐标**：`rv.c:71`（`rv_csr_bus`，注意读写掩码表）、`rv.h` 行 59-66（`rv_csr` 结构体）、`rv.c:607`（`csrrw` 系列）。
- **突破点**：`mstatus` 既是 `0x300` 又是 `0x100`（sstatus 是它的只读视图），所以用「读掩码 / 写掩码」两列来表达权限，而不是写两套逻辑；写 `satp` 还额外受 `mstatus.tvm` 约束。
- **通过标准**：能解释为什么读 `sstatus` 和读 `mstatus` 走同一行代码却得到不同结果；能说出 `csrrs` / `csrrc` 在 `rs1 == x0`（或不写回）时的特殊语义。

## todoE 内存接口：CPU 核与「世界」的唯一接触面

- **目标**：理解 `rv_bus_cb` 这个只有 6 行的契约，为什么足以支撑整个平台。
- **代码坐标**：`rv.h` 行 72-75（回调类型）、`rv.c:387`（`rv_bus`）、`rv.c:369`（`rv_endcvt`）、`rv.c:411`（`rv_if`）。
- **突破点**：`rv_bus` 负责对齐检查、地址翻译、**跨页访问拆成两次**；端序转换由 `rv_endcvt` 单独承担（宿主可能是大端）；取指为了准确报告 fault 地址还要分两次 2 字节读。
- **通过标准**：能说明一次跨 4KB 边界的 4 字节访问在 `rv_bus` 里发生了什么、为什么必须重新调一次 `rv_vmm`；能写出一个只支持 32 位对齐访问的 `bus_cb` 会破坏哪些测试。

## todoF 地址翻译 sv32：Linux 的硬门槛

- **目标**：彻底掌握两级页表遍历、权限检查与 TLB。
- **代码坐标**：`rv.c:155`（`rv_vmm`）、`rv.c:651`（`sfence.vma`）、`rv.h` 行 86（`tlb_*` 字段）。
- **突破点**：这是全仓库信息密度最高的一段——`U`/`SUM`/`MXR` 权限、巨页对齐、`pte.a`/`pte.d` **不由硬件置位**（不置位就抛缺页，交给 Linux 自己处理）、单条目 TLB 缓存与 `sfence.vma` 失效。
- **通过标准**：能画出 sv32 两级遍历过程；能解释「把 `pte.d` 自动置 1」会改变哪个行为的正确性；能说明 `satp.mode == 0` 时 `rv_vmm` 退化成什么。

## todoG 32 位机器上的 64 位算术

- **目标**：理解 RV32 内核为什么也需要 `mulh` / `mulhu` / `mulhsu`，以及如何只靠 32 位类型实现。
- **代码坐标**：`rv.c:205`（16 位加法）、`rv.c:212`（16 位乘法）、`rv.c:219`（32×32→64）、`rv.c:581`（M 扩展分派）、`rv.c:591`（`div`/`rem` 的零除与溢出边界）。
- **突破点**：`rvm` 把 32 位拆成 4 个 16 位部分积，用手工进位链拼出 64 位；`div` 除零返回 -1、`rem` 除零返回被除数，`INT_MIN / -1` 也要特判。
- **通过标准**：能手算 `rvm` 中 `m/n/z/w` 各步的进位含义；能说出 `div` 与 `divu` 在 `b == 0` 时返回值的区别。

## todoH 原子操作与 LR/SC

- **目标**：弄懂 `lr.w` / `sc.w` 的保留集语义，以及为什么它是 Linux 内核锁的基础（没有 A 扩展 Linux 根本起不来）。
- **代码坐标**：`rv.c:509`（AMO 段）、`rv.h` 行 85（`res`, `res_valid`）、`rv.c:523`（`lr.w`）、`rv.c:525`（`sc.w`）。
- **突破点**：`sc.w` 成功返回 0、失败返回 1，且成功与否取决于保留集（本实现是「单次有效」的 `res_valid--`）；`amomin/amomax` 借助 `rv_ovf` 处理有符号比较。
- **通过标准**：能解释为什么 `sc.w` 失败时不能写内存；能说出把 `res_valid` 改成不清零会导致什么问题。

## todoI 中断：从 CLINT 到 PLIC 两条路径

- **目标**：分清「核内的定时器 / IPI」与「核外的设备中断」这两套机制。
- **代码坐标**：`tools/linux/rv_clint.c`、`tools/linux/rv_plic.c`、`rv.c:682`（`rv_irq`）、`rv.c:434`（`rv_service`）、`rv.h` 行 70（`RV_CSI/CTI/CEI`）。
- **突破点**：CLINT 提供 `mtime` / `mtimecmp` / `mswi`（Linux 用定时器做调度、用 MSI 做多核 IPI）；PLIC 是「读 claim 寄存器 = 取中断号，写回 = complete」的两阶段协议，还有 priority / thresh 的过滤链。
- **通过标准**：能说出 `mach.c` 主循环里 `rv_uart_update` → `rv_plic_irq` → `rv_irq` 这条链路每个函数做了什么；能解释 PLIC 里 `claiming` 位存在的意义。

## todoJ UART：控制台是怎么冒出来的

- **目标**：掌握一个真实外设的寄存器模型与「轮询 + 中断」两种工作模式。
- **代码坐标**：`tools/linux/rv_uart.c`、`tools/linux/rv_uart.h`、`mach.c:51`（`uart0_io`）、`mach.c:72`（`load`）。
- **突破点**：`rv_uart_update` 按 `div` 分频模拟波特率，`txctrl`/`rxctrl` 低位是收发使能、高位是比较阈值，`ie`/`ip` 决定是否向 PLIC 上报；`mach.c` 用 ncurses 的 `getch`/`echochar` 把模拟器接到你的键盘和屏幕。
- **通过标准**：能解释把 `div` 改大会有什么后果、`noecho` 与 `echochar` 在 `uart0_io` 里的分工；能说出 Linux 看到的是哪个 `compatible` 字符串。

## todoK 平台总装：mach.c 是那块主板

- **目标**：把前面所有零件拼成一台机器，理解地址映射与主循环。
- **代码坐标**：`tools/linux/mach.c` 行 10-17（内存映射）、`mach.c:28`（`mach_bus`）、`mach.c:82`（`main`）、`tools/linux/Makefile`。
- **突破点**：`mach_bus` 就是「地址解码器」，按区间把访问转发给 RAM / PLIC / CLINT / UART；主循环里 `rtc_period` 每 0x1000 拍推进一次 `mtime`；`a0 = hartid`、`a1 = dtb 指针` 是给固件的启动约定。
- **通过标准**：能对着 `mach.c` 常量与 `rv.dts` 的 `reg` 逐项核对一致；能说出若 RAM 映射少了 4KB 会在启动的哪个阶段报错。

## todoL 设备树：内核怎么知道外面有什么

- **目标**：理解 dtb 为什么是「可移植内核」的关键，而不是可有可无的配置。
- **代码坐标**：`tools/linux/extern/rv.dts`、`mach.c:108`（dtb 加载到 32MiB 处）、`tools/linux/extern/configs/rv_defconfig`（DTS 相关项）。
- **突破点**：`compatible = "sifive,uart0"` 与内核 `CONFIG_SERIAL_SIFIVE` 驱动匹配；`timebase-frequency` 决定内核的 `clocksource`；`interrupt-parent`/`interrupts` 把 UART 接到 PLIC；`bootargs` 里的 `earlycon` 决定了「最早那一行日志」能不能出来。
- **通过标准**：能解释删掉 `chosen/stdout-path` 会发生什么；能把 `rv.dts` 里每个节点映射回 `mach.c` 中的一段代码。

## todoM 固件层：OpenSBI 与引导约定

- **目标**：搞清楚 `fw_payload.bin` 里到底装了谁，以及 M→S 的交接细节。
- **代码坐标**：`tools/linux/README.md`（构建命令）、`rv_defconfig` 中 `BR2_TARGET_OPENSBI*`、`mach.c:122`（启动寄存器约定）。
- **突破点**：OpenSBI 在 M 模式初始化并写 `medeleg`/`mideleg` 后跳到 Linux（S 模式），之后 Linux 通过 SBI 调用请求定时器、控制台、IPI；`PLATFORM_RISCV_ISA=rv32imaczicsr_zifencei` 说明 CSR 与 `fence.i` 是独立扩展，对应 `rv.c` 里的 `rv_csr_bus` 和 `fence.i` 分支。
- **通过标准**：能说出「为什么必须有个 M 模式固件」；能列出 Linux 至少 3 种会退化成 SBI 调用的操作。

## todoN 内核与根文件系统：Linux 为什么能被塞进 128MiB

- **目标**：理解 buildroot 配置里的每个开关与「能跑起来」的因果关系。
- **代码坐标**：`rv_defconfig`、`tools/linux/extern/Kconfig`。
- **突破点**：`BR2_RISCV_32 + USE_MMU + ILP32` 决定了内核是 RV32；`INITRAMFS` 意味着没有磁盘、根文件系统打包进内核镜像；`CONFIG_ARCH_RV32I`、`CONFIG_RISCV_ISA_C`、`CONFIG_SIFIVE_PLIC`、`CONFIG_SERIAL_SIFIVE` 每一条都对应模拟器里的一段实现。
- **通过标准**：能逐条回答「关掉这个 CONFIG 会怎样」；能说出为什么 RV32 内核必须要有 MMU 与 C 扩展。

## todoO 端到端时序：把整条链路串成一条时间线

- **目标**：合上代码，完整复述加电到 shell 的过程，标出每个交接点。
- **代码坐标**：`mach.c:82` 起、`rv.c:447`、`rv_clint.c`、`rv_plic.c`、`rv.dts`。
- **突破点**：模拟器先跑的是 OpenSBI（M 模式），而不是 Linux；定时器中断由模拟器主循环推进 `mtime` 产生，而不是真实时钟；控制台回显来自 `uart0_io`，与 Linux 的 `ttySIF0` 之间隔着 PLIC 和 SBI 两层。
- **通过标准**：能画出一条带分支的时间线，并在 `rv_step` / `rv_trap` / `mach_bus` 上标出各阶段会命中的代码位置。

## todoP 测试与调试：怎么证明「我改的东西是对的」

- **目标**：掌握仓库自带的验证手段，形成自己的实验闭环。
- **代码坐标**：`tools/test/run_test.c`（通过判据 `r[3] == 1 && r[10] == 0`）、`tools/test/Makefile`、`tools/test/regresh.c`、`tools/scripts/dumpy.py`、`tools/test/test.py`。
- **突破点**：`riscv-tests` 用 `tohost` 约定汇报结果；`regresh.c` 是「回归特定 issue」的微型测试；`dumpy.py` 读 `/*I ... */` 标注生成 README 的指令索引——你新增指令时应当补标注。
- **通过标准**：能跑通 `tools/test` 并解释 `r[3]`、`r[10]` 的含义；能自己写一个针对 `todoF` 或 `todoH` 的微型回归测试。

## todoQ 边界与缺口：距离「跑真实发行版」还差什么

- **目标**：知道这套实现的取舍边界，避免把模拟器行为当成硬件规范。
- **代码坐标**：`README.md` 的 Caveats、`rv.c:155`（A/D 位）、`rv.c` 行 386-408（无缓存模型）、`rv_clint.c:37`（单核假设）。
- **突破点**：无 64 位、无 A/D 自动置位、单条目 TLB、无 ASID、单核、内存模型无缓存一致性、无设备直通（uart1 是空实现）——每一条都对应一个「换成多核/大内存/高速设备就会出问题」的边界。
- **通过标准**：能列出至少 5 项缺口，并说明每项在什么场景下会变成真正的 bug。

---

## 加速建议

- 全程配一个「三栏笔记本」：**RISC-V 规范条款 / 本仓库实现位置 / 我能验证它的最小实验**。清单里的每一项都应填满三栏才算过关。
- 卡住时的顺序：先读 `rv.h` 的数据结构，再看 `rv.c` 的对应函数，最后去 `mach.c` 找它的驱动者。
- 每完成一项，就在对应标题后追加 `[x]`（例如 `## todoC [x] trap 与特权级`），并在下面用一两行记下「我以为 X，实际是 Y」——这些认知反转点才是这份清单的价值所在。
