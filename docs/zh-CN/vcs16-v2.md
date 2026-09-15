# vCS-16/2

vCS-16/2 是 SCMD 仓库中的 **16-bit 虚拟执行架构**。它不再试图复刻现实 CPU 的微结构；`16` 描述架构数据宽度和虚拟地址语义，而不是要求每个 backend 都实现真实的 fetch/decode/FLAGS/MMIO 电路。

旧 `examples/vcs16-a1` 保留为历史/兼容实验。A1 证明了在 CFG alias runtime 中模拟传统 16-bit CPU 可行，但真实 CPU 风格的 ROM fetch、opcode decode、FLAGS 和寄存器选择会付出非常高的 alias 成本，不适合作为长期系统 ABI。

## 核心原则

- 架构语义与物理实现分离。
- 8 个 16-bit architectural register：`r0..r7`。
- 没有 architectural FLAGS；条件跳转直接比较寄存器。
- `CALL/RET`、`LD8/ST8`、`SYS` 是架构能力。
- Reference VM 可以逐指令解释。
- SCMD AOT backend 不做 runtime opcode fetch/decode，而把已知指令直接 lowering 成 SCMD 控制/数据流。
- Host/OS 通过 `SYS` 提供 TTY、VFS、task 等服务；vCS core 本身不知道 CS2。

## 当前指令

```text
nop
mov rA, rB
ldi rA, imm16
add/sub/and/or/xor rA, rB
addi/subi rA, imm16
ld8/st8 rA, rBase, offset16
jmp label
jeq/jne/jlt/jge rA, rB, label
call label
ret
sys imm16
halt
```

`jlt/jge` 当前是 unsigned 16-bit compare。

## 汇编指令

```text
.memory 256
.entry start
```

例：

```asm
.memory 64
.entry start
start:
    ldi r0, 40
    call add_two
    sys 2
    halt
add_two:
    addi r0, 2
    ret
```

## libvcs16

公共 C ABI：

```c
#include <vcs16/vcs16.h>
```

主要能力：

- `vcs16_assemble`
- `vcs16_module_verify`
- `vcs16_module_save/load`（VXE2）
- `vcs16_vm_create/reset/step/run`
- host syscall / trap callbacks
- register / PC / memory inspection

CLI 只是库的薄封装：

```text
vcs16as    source.vcs -> executable.vxe
vcs16run   reference VM
vcs16dump  VXE2 disassembly/metadata dump
```

CMake 同时提供真正可链接的 `vcs16` / `vcs16_scmd` targets；`cmake --install` 会安装 `libvcs16`、`libvcs16_scmd` 和 `include/vcs16/` 公共头文件，所以它不是由 CLI 反向包装出来的“假库”。

## libvcs16_scmd

```c
#include <vcs16/scmd.h>
```

`vcs16_scmd_write_source()` 把同一个 vCS module AOT-lower 成 SCMD 源码。

CLI：

```text
vcs16scmd input.vcs -o generated.scmd --prefix app
```

生成代码公开：

```scmd
export function app_run()
```

并要求 host 实现：

```scmd
function app_host_syscall()
```

SYS 参数可从：

```text
app_r0_lo/app_r0_hi ... app_r7_lo/app_r7_hi
app_sys_lo/app_sys_hi
```

读取。Host 将返回值写入：

```text
app_sysret_lo/app_sysret_hi
```

AOT runtime 随后提交到 architectural `r0`，与 reference VM 的 syscall return 语义一致。

### AOT 当前实现

AOT backend 已覆盖当前 vCS-16/2 指令，包括 `CALL/RET` 和动态 `LD8/ST8`。byte-addressed backing 当前限制为最多 256 B；这是 SCMD AOT implementation limit，不是 vCS 架构地址上限。

AOT 仍保留 architectural PC 以便调试/验证，但**不会运行 ROM fetch/opcode decode**。后续可以继续把 PC dispatcher 优化为 basic-block continuation，而不改变 VXE2/ABI。

## VXE2

当前 executable magic：

```text
VXE2
```

保存：

- architecture version
- ABI version
- entry
- virtual memory byte count
- fixed-width instruction stream

加载时必须经过 verifier。

