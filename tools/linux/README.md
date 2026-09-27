# Linux 机器

本目录包含构建并运行一个面向 `rv` 的最小 Linux 内核所需的全部内容。

[`mach.c`](mach.c) 实现了一台基本机器，包含以下硬件：
- [`rv.c`](../../rv.c) RISC-V CPU 核心（废话）
- [`rv_clint.c`](rv_clint.c) RISC-V 核心本地中断器 (CLINT)
- [`rv_plic.c`](rv_plic.c) RISC-V 平台级中断控制器 (PLIC)
- [`rv_uart.c`](rv_uart.c) SiFive 通用异步收发器 (UART)（共 2 个）

## 构建
恭喜，现在轮到你来编译 Linux 了！

```shell
# [所有命令都在本目录 (rv/tools/linux) 下执行]
# 下载并解压 buildroot
curl -L https://github.com/buildroot/buildroot/archive/refs/tags/2023.11.1.tar.gz -o buildroot.tar.gz
tar -xf buildroot.tar.gz && mv buildroot*/ buildroot/
# 构建 linux（这会花很长时间）
make -C buildroot BR2_EXTERNAL=$(realpath extern) rv_defconfig 
make -C buildroot 
# 再构建一次 linux 以修复 initrd 问题（应该不会太久）
make -C buildroot linux-rebuild opensbi-rebuild all
# 构建机器
make mach
# 运行机器
./mach buildroot/output/images/fw_payload.bin buildroot/output/images/rv.dtb
```
