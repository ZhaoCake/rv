# rv

用 ANSI C 编写的 RISC-V CPU 核心。

特性：

- 实现了 `RV32IMAC_Zicsr`，支持 M 模式和 S 模式
- 可以启动 RISCV32 Linux
- 通过 [`riscv-tests`](https://github.com/riscv/riscv-tests) 中所有受支持的测试
- 约 800 行代码
- 不使用任何大于 32 位的整数类型，乘法也是如此
- 简洁的 API（两个必需函数，外加一个由你提供的内存回调函数）
- 无内存分配

## API

```c
/* 内存访问回调：data 既是输入也是输出，出错时返回 RV_BAD。 */
typedef rv_res (*rv_bus_cb)(void *user, rv_u32 addr, rv_u8 *data, rv_u32 is_store, rv_u32 width);

/* 初始化 CPU。可以对 `cpu` 再次调用以复位。 */
void rv_init(rv *cpu, void *user, rv_bus_cb bus_cb);

/* 单步执行 CPU。发生异常时返回 RV_E*。 */
rv_u32 rv_step(rv *cpu);
```

## 用法

```c
#include <stdio.h>
#include <string.h>

#include "rv.h"

#define RAM_BASE 0x80000000
#define RAM_SIZE 0x10000

rv_res bus_cb(void *user, rv_u32 addr, rv_u8 *data, rv_u32 is_store,
              rv_u32 width) {
  rv_u8 *mem = (rv_u8 *)user + addr - RAM_BASE;
  if (addr < RAM_BASE || addr + width >= RAM_BASE + RAM_SIZE)
    return RV_BAD;
  memcpy(is_store ? mem : data, is_store ? data : mem, width);
  return RV_OK;
}

rv_u32 program[2] = {
    /*            */             /* _start: */
    /* 0x80000000 */ 0x02A88893, /* add a7, a7, 42 */
    /* 0x80000004 */ 0x00000073  /* ecall */
};

int main(void) {
  rv_u8 mem[RAM_SIZE];
  rv cpu;
  rv_init(&cpu, (void *)mem, &bus_cb);
  memcpy((void *)mem, (void *)program, sizeof(program));
  while (rv_step(&cpu) != RV_EMECALL) {
  }
  printf("Environment call @ %08X: %u\n", cpu.csr.mepc, cpu.r[17]);
  return 0;
}
```

参见 [`tools/example/example.c`](tools/example/example.c)。

## 运行 Linux

本仓库包含一个可以使用 `rv` 启动 Linux 的机器模拟器。
参见 [`tools/linux/README.md`](tools/linux/README.md)。

## 面向 `rv` 编译

配合 [tools/link.ld](tools/link.ld) 使用 [riscv-gnu-toolchain](https://github.com/riscv-collab/riscv-gnu-toolchain)。

建议的 GCC 命令行：

`riscv64-unknown-elf-gcc example.S -nostdlib -nostartfiles -Tlink.ld -march=rv32imac -mabi=ilp32 -o example.o -e _start -g -no-pie`

要导出一个起始于 `0x80000000`、可被 `rv` 直接加载的二进制文件（如上例所示）：

`riscv64-unknown-elf-objcopy -g -O binary example.o example.bin`

## 指令列表

点击某条指令即可查看它在 `rv.c` 中的实现。

- [`add       `](rv.c#L560)[`addi      `](rv.c#L560)[`amoadd.w  `](rv.c#L519)[`amoand.w  `](rv.c#L531)[`amomax.w  `](rv.c#L535)[`amomaxu.w `](rv.c#L539)[`amomin.w  `](rv.c#L533)[`amominu.w `](rv.c#L537)
- [`amoor.w   `](rv.c#L529)[`amoswap.w `](rv.c#L521)[`amoxor.w  `](rv.c#L527)[`and       `](rv.c#L577)[`andi      `](rv.c#L577)[`auipc     `](rv.c#L669)[`beq       `](rv.c#L480)[`bge       `](rv.c#L483)
- [`bgtu      `](rv.c#L485)[`blt       `](rv.c#L482)[`bltu      `](rv.c#L484)[`bne       `](rv.c#L481)[`c.add     `](rv.c#L357)[`c.addi    `](rv.c#L295)[`c.addi16sp`](rv.c#L302)[`c.and     `](rv.c#L323)
- [`c.andi    `](rv.c#L314)[`c.beqz    `](rv.c#L333)[`c.bnez    `](rv.c#L335)[`c.ebreak  `](rv.c#L354)[`c.j       `](rv.c#L331)[`c.jal     `](rv.c#L297)[`c.jalr    `](rv.c#L351)[`c.jr      `](rv.c#L346)
- [`c.li      `](rv.c#L299)[`c.lui     `](rv.c#L304)[`c.lw      `](rv.c#L287)[`c.lwsp    `](rv.c#L343)[`c.mv      `](rv.c#L348)[`c.or      `](rv.c#L321)[`c.slli    `](rv.c#L341)[`c.srai    `](rv.c#L312)
- [`c.srli    `](rv.c#L310)[`c.sub     `](rv.c#L317)[`c.sw      `](rv.c#L289)[`c.swsp    `](rv.c#L359)[`c.xor     `](rv.c#L319)[`csrrc     `](rv.c#L622)[`csrrci    `](rv.c#L622)[`csrrs     `](rv.c#L616)
- [`csrrsi    `](rv.c#L616)[`csrrw     `](rv.c#L607)[`csrrwi    `](rv.c#L607)[`div       `](rv.c#L591)[`divu      `](rv.c#L593)[`ebreak    `](rv.c#L658)[`ecall     `](rv.c#L655)[`fence     `](rv.c#L502)
- [`fence.i   `](rv.c#L506)[`jal       `](rv.c#L548)[`jalr      `](rv.c#L495)[`lb        `](rv.c#L459)[`lbu       `](rv.c#L459)[`lh        `](rv.c#L459)[`lhu       `](rv.c#L459)[`lr.w      `](rv.c#L523)
- [`lui       `](rv.c#L671)[`lw        `](rv.c#L459)[`mret      `](rv.c#L631)[`mul       `](rv.c#L581)[`mulh      `](rv.c#L581)[`mulhsu    `](rv.c#L581)[`mulhu     `](rv.c#L581)[`or        `](rv.c#L575)
- [`ori       `](rv.c#L575)[`rem       `](rv.c#L595)[`remu      `](rv.c#L597)[`sb        `](rv.c#L471)[`sc.w      `](rv.c#L525)[`sfence.vma`](rv.c#L651)[`sh        `](rv.c#L471)[`sll       `](rv.c#L565)
- [`slli      `](rv.c#L565)[`slt       `](rv.c#L567)[`slti      `](rv.c#L567)[`sltiu     `](rv.c#L569)[`sltu      `](rv.c#L569)[`sra       `](rv.c#L573)[`srai      `](rv.c#L573)[`sret      `](rv.c#L631)
- [`srl       `](rv.c#L573)[`srli      `](rv.c#L573)[`sub       `](rv.c#L560)[`sw        `](rv.c#L471)[`wfi       `](rv.c#L648)[`xor       `](rv.c#L571)[`xori      `](rv.c#L571)

## 常见问题

### 代码像意大利面一样乱！

- `rv` 的编写方式充分利用了 RISC-V 指令的正交性。
- `rv` 也努力在简洁与可读性之间取得良好平衡。
- 当然，要读懂这段代码，至少需要对 ISA 编码有深入的先备知识。

### 没有 switch 语句！

- C 语言只允许在 switch 语句中使用常量表达式。在作者看来，除了大量 `break` 语句之外再用上它们，只会让代码更加臃肿。事实证明，你完全可以改用 switch 语句重新实现这段代码。参见 [LICENSE.txt](LICENSE.txt)。

### 没什么用！

- [好](https://www.google.com/search?q=happy+smiley+thumbs+up+happy+cool+funny+ok&tbm=isch)

### 太慢了！

- [好](https://www.google.com/search?q=happy+smiley+thumbs+up+happy+cool+funny+ok&tbm=isch)

## 注意事项

- 采用 C89 编写。
- 其实并不完全符合 C89，因为它使用了长度超过 6 个字符的外部名称。
- 不使用任何大于 32 位的整数类型，乘法也是如此，因为它采用 C89 编写。
- 以不完全符合 C89/99 的方式假设整数类型的宽度。相关修复即将到来，我正在为 C89 打造一个滴水不漏的 `<stdint.h>`。
- 采用 C89 编写。
