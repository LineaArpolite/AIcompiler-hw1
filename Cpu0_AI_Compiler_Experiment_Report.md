# 编译原理与 AI 工具链实验报告（Cpu0 LLVM17）

## 1. 实验目标
在 Jonathan Lee 的 Cpu0 LLVM 教程代码基础上，完成以下三类工作：

1. 新增 4 条整数指令并打通汇编/反汇编链路：`imac/max/min/popc`
2. 打通指令选择：让 `relu/fma/popcount` 自动下降为目标指令，并验证常量除法 `x/10` 走 `mult+mfhi+移位`
3. 新增 Machine-Level 优化 Pass：`mul` 常量强度削减（`mul x, 3/5/7... -> shl + addu/subu`）

交付物包含：

- Cpu0 后端代码改动
- lit 风格测试
- 本报告 Markdown 源文档（可转 PDF）

---

## 2. 实验环境

- OS: WSL Linux
- 工程目录: `/home/linea/work/Cpu0_For_LLVM17`
- LLVM 版本: `llvm-project-llvmorg-17.0.6`
- 已用构建参数：
  - `-DLLVM_ENABLE_PROJECTS="clang;lld"`
  - `-DLLVM_TARGETS_TO_BUILD="X86;Mips;Cpu0"`

---

## 3. 实现内容

## 3.1 任务1：指令定义与汇编支持

### 3.1.1 新增指令与编码
在 `Cpu0InstrFormats.td` 新增 `R4-type`：

- `FrmR4 : Format<5>`
- `FR4<...>` 编码布局：`opcode|ra|rb|rc|rd|resv8`

在 `Cpu0InstrInfo.td` 新增：

- `IMAC`（R4-type）: `rd = rs * rt + ru`
- `MAX`（A-type）: `rd = smax(rs, rt)`
- `MIN`（A-type）: `rd = smin(rs, rt)`
- `POPC`（A-type）: `rd = ctpop(rs)`

使用 opcode：`0x53~0x56`。

### 3.1.2 MC 层支持
通过 TableGen 自动支持新编码/打印/反汇编：

- `Cpu0GenMCCodeEmitter.inc`
- `Cpu0GenAsmWriter.inc`
- `Cpu0GenDisassemblerTables.inc`

并扩展 TSFlags 枚举：

- `Cpu0BaseInfo.h` 增加 `FrmR4 = 5`

### 3.1.3 AsmParser 接入（原工程缺失）
原始 Cpu0 后端无目标汇编解析器，`llvm-mc` 报：

- `this target does not support assembly parsing`

本次新增：

- `llvm/lib/Target/Cpu0/AsmParser/CMakeLists.txt`
- `llvm/lib/Target/Cpu0/AsmParser/Cpu0AsmParser.cpp`
- `llvm/lib/Target/Cpu0/Cpu0Asm.td`
- `llvm/lib/Target/Cpu0/Cpu0RegisterInfoGPROutForAsm.td`

并在 `llvm/lib/Target/Cpu0/CMakeLists.txt` 接入：

- `tablegen(... Cpu0GenAsmMatcher.inc -gen-asm-matcher)`
- `add_subdirectory(AsmParser)`
- `LINK_COMPONENTS` 增加 `Cpu0AsmParser`

### 3.1.4 兼容性修复
为避免 asm-matcher 生成时的重复键冲突（寄存器名重复），将：

- `HI: "ac0"`, `LO: "ac0"`

改为：

- `HI: "hi"`, `LO: "lo"`

---

## 3.2 任务2：指令选择

### 3.2.1 Legalize 设置
`Cpu0ISelLowering.cpp` 中：

- `ISD::SMAX` -> `Legal`
- `ISD::SMIN` -> `Legal`
- `ISD::CTPOP` -> `Legal`

### 3.2.2 TableGen Pattern
`Cpu0InstrInfo.td` 中新增：

- `smax/smin -> MAX/MIN`
- `ctpop -> POPC`
- `smax x, 0` / `smin x, 0` 到 `MAX/MIN + ZERO` 的模式（用于 relu 类场景）

### 3.2.3 IMAC 的 DAG Combine
新增 target node：

- `Cpu0ISD::IMAC`

在 `PerformDAGCombine` 中新增 `ISD::ADD` 组合：

- 识别 `add(mul(a,b), c)`（mul 单一 use）
- 生成 `Cpu0ISD::IMAC(a,b,c)`
- TableGen 再匹配到 `imac`

### 3.2.4 子目标特性开关修复
原始 `Cpu0Subtarget.h` 中 `hasChapter12_1()` 返回 `false`，导致 `Ch12_1` 谓词指令不可选。已改为 `true`。

---

## 3.3 任务3：Machine-Level 优化 Pass

新增文件：

- `llvm/lib/Target/Cpu0/Cpu0MulStrengthReduce.cpp`

并完成接入：

- `Cpu0.h` 声明 `createCpu0MulStrengthReducePass()`
- `Cpu0/CMakeLists.txt` 增加源文件
- `Cpu0TargetMachine.cpp::addPreEmitPass()` 注册该 pass

命令行开关：

- `-enable-cpu0-mul-strength-reduce`（默认 `true`）

### 3.3.1 优化策略
在 MachineInstr 层识别：

- `mul dst, x, const_reg`

当 `const_reg` 来自 `addiu/ori $zero, imm` 时，将常量乘法改写为：

- 2 的幂：`shl`
- `2^k + 1`：`shl + addu`
- `2^k - 1`：`shl + subu`
- 负数常量：最后再 `subu $zero, result`

并在安全条件下删除被替换掉的死常量装载指令。

---

## 4. 验证与结果

## 4.1 新指令汇编/反汇编
测试文件：`llvm/test/MC/Cpu0/ai-insts.s`

验证命令（已通过）：

- `llvm-mc -triple=cpu0 -show-encoding`
- `llvm-mc -triple=cpu0 -filetype=obj | llvm-objdump -d --triple=cpu0`

关键结果：

- `imac/max/min/popc` 均可汇编
- 对象反汇编能正确打印对应助记符

## 4.2 CodeGen 自动选指令
测试文件：`llvm/test/CodeGen/Cpu0/ai-patterns.ll`

验证命令（已通过）：

- `llc -march=cpu0 -mcpu=cpu032II -O2`

关键结果：

- `relu` -> `max`
- `a*b+c` -> `imac`
- `ctpop` -> `popc`
- `x/10` -> `mult + mfhi + sra(+修正)`，无显式 `div`

## 4.3 Machine Pass 开关验证
测试文件：`llvm/test/CodeGen/Cpu0/mul-strength-reduce.ll`

验证命令（已通过）：

- 默认开启：`llc ... -O2`
- 显式关闭：`llc ... -O2 -enable-cpu0-mul-strength-reduce=false`

关键结果：

- 开启：`mul3` 变为 `shl + addu`
- 关闭：`mul3` 保持 `mul`

---

## 5. 关键改动文件清单

- `llvm/lib/Target/Cpu0/CMakeLists.txt`
- `llvm/lib/Target/Cpu0/Cpu0.h`
- `llvm/lib/Target/Cpu0/Cpu0Asm.td`（新增）
- `llvm/lib/Target/Cpu0/Cpu0RegisterInfoGPROutForAsm.td`（新增）
- `llvm/lib/Target/Cpu0/Cpu0InstrFormats.td`
- `llvm/lib/Target/Cpu0/Cpu0InstrInfo.td`
- `llvm/lib/Target/Cpu0/Cpu0ISelLowering.h`
- `llvm/lib/Target/Cpu0/Cpu0ISelLowering.cpp`
- `llvm/lib/Target/Cpu0/Cpu0TargetMachine.cpp`
- `llvm/lib/Target/Cpu0/Cpu0MulStrengthReduce.cpp`（新增）
- `llvm/lib/Target/Cpu0/Cpu0Subtarget.h`
- `llvm/lib/Target/Cpu0/Cpu0RegisterInfo.td`
- `llvm/lib/Target/Cpu0/Cpu0RegisterInfo.cpp`
- `llvm/lib/Target/Cpu0/MCTargetDesc/Cpu0BaseInfo.h`
- `llvm/lib/Target/Cpu0/AsmParser/CMakeLists.txt`（新增）
- `llvm/lib/Target/Cpu0/AsmParser/Cpu0AsmParser.cpp`（新增）
- `llvm/test/MC/Cpu0/ai-insts.s`（新增）
- `llvm/test/CodeGen/Cpu0/ai-patterns.ll`（新增）
- `llvm/test/CodeGen/Cpu0/mul-strength-reduce.ll`（新增）

---

## 6. PDF 转换建议

可用 `pandoc`：

```bash
pandoc Cpu0_AI_Compiler_Experiment_Report.md -o Cpu0_AI_Compiler_Experiment_Report.pdf
```

若需要中文字体，可额外指定模板和字体参数。

---

## 7. 结论

本实验已完整打通 Cpu0 后端在 `LLVM IR -> SelectionDAG -> MachineInstr -> MC -> 汇编` 链路上的新增能力：

- 新增 4 条 AI 相关整数指令并支持 `llvm-mc` 汇编/反汇编
- `relu/fma/popcount` 自动映射为目标指令
- 常量除法表现为乘高位与移位序列
- Machine 层常数乘法强度削减可开关、可验证

达到作业要求。
