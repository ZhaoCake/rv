RVOD=riscv64-unknown-elf-objdump
RVOC=riscv64-unknown-elf-objcopy
CC=cc
RISCV_TESTS=$(RISCV)/target/share/riscv-tests
CFLAGS=--std=c89 -Wall -Wextra -pedantic -Wshadow -g

all: test

test: vectors run_test
	python test.py

vectors:
	mkdir -p vectors
	cp $(RISCV_TESTS)/isa/rv32ui-p-* vectors
	cp $(RISCV_TESTS)/isa/rv32si-p-* vectors
	cp $(RISCV_TESTS)/isa/rv32mi-p-* vectors
	cp $(RISCV_TESTS)/isa/rv32ua-p-* vectors
	# rv32uc（C 扩展）与 rv32um（M 扩展）的向量已移除：本核心不再实现这两个扩展
	rm -rf vectors/rv32mi-p-breakpoint # 不支持断点
	rm -rf vectors/rv32ui-p-ma_data    # 不支持非对齐加载/存储
	rm -rf vectors/*.dump
	find vectors -not -path vectors -name '*' ! -name '*.dmp' -exec bash -c "$(RVOD) -D -M no-aliases -M numeric '{}' > '{}.dmp'" \;
	find vectors -not -path vectors -name '*' ! -name '*.dmp' -exec bash -c "$(RVOC) -O binary '{}' '{}'" \;

run_test: run_test.c rv.c
	$(CC) $(CFLAGS) run_test.c rv.c -o $@

rv32%: vectors run_test
	./run_test vectors/$@

regresh: regresh.c rv.c
	$(CC) $(CFLAGS) regresh.c rv.c -o $@

run_regresh: regresh
	./regresh

clean:
	rm -rf vectors run_test run_test_debug *.dSYM
