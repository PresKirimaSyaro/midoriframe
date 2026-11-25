# mdrframe

`mdrframe` 是五级顺序流水线指令 DSL 的 C++20 前端和中端。它读取单个 `.mdr` 指令集，输出带阶段的中间表示和依赖表。目标架构沿用 P5 的 F/D/E/M/W 流水线。当前输出可供后端使用，Verilog 工程生成和时序仿真属于后续工作。

## 构建与运行

需要 CMake 3.20 或更新版本，以及支持 C++20 的编译器。在当前 Windows 环境可用 MinGW。

```powershell
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
.\build\mdrframe.exe examples\p5.mdr
.\build\mdrframe.exe --emit=deps examples\conditional.mdr
.\build\mdrframe.exe --emit=ir examples\lrm.mdr -o build\lrm.mdrir
```

在其他环境可以省略 `-G` 或选择本机 CMake 生成器。默认输出完整 IR 到标准输出。`--emit=deps` 保留解释依赖所需的表达式，省去无关节点和源码坐标。若输入名没有扩展名且路径不存在，程序尝试附加 `.mdr`。显式提供路径时也可以读取先前的 `.dsl` 样例。

P5 默认使用二参数整字写 `dm_write(addr, val)`，整字访存忽略地址低两位。若要分析早期 P6 样例中的字节或半字写，显式加 `--p6-narrow-writes`。

## 输出约定

- `priority` 按源文件顺序编号，首个命中者生效。译码未命中时无副作用。
- `values` 是带宽度、符号、计算级和操作数的表达式图。`fit` 标记 Verilog 表达式的赋值位宽上下文，保留源运算符和定宽边界。算术节点上的宽度是它自身的推导宽度，后端仍需按使用处的 Verilog 上下文延展计算。静态循环已展开，`phi` 合并分支值，`insert` 更新位或切片。一个 IR 值编号只定义一次。
- `effects` 给出带执行条件的有序动作，包括访存、跳转、清槽和 Promise 更新。`%N` 引用值，`#N` 引用动作。内存及 GPR 写回的数据和目的可由不同阶段产生。
- `register_reads` 给出 D 级可计算的读号、读使能和实际使用级。D/E/M 的 `Tuse` 分别为 0/1/2。只依赖编码或 PC 的条件可用于 D 级读口选择，依赖 GPR 或 DM 的条件保守处理。读口上界按整份指令集推导，可合并等价的读号，`$0` 可直接取零。
- `pipeline_carries` 列出 D→E、E→M 之间需要传递的值。普通值已完成计算。标为 `rf-address+candidate` 的操作数还要随级接受旁路更新。IR 中的 `ins` 和 `pc` 由后端作为每条流水指令的公共信息提供。
- `promise` 给出初始 RAW 候选上界及各路径的 `update`、`yield` 和保证就绪的边界。首次 `update` 赋值，后续取交集。零 bitmap 无写回。路径未定时取剩余可能路径中最迟的可用边界，已确定时按该路径判断。未枚举的复杂情况仍有有序受保护动作可供后端分析。`ready_by` 按有效的单目标写回程序计算；最终多位的行为依照 DSL 规范未定义。

`examples/p5.mdr` 包含 16 条 P5 基础指令。`examples/lrm.mdr` 由内存数据决定写回哪一个 GPR。`examples/conditional.mdr` 展示同一条指令在 E→M 或 M→W 就绪的两条路径。

编译期求值支持最多 64 位的常量计算，较宽的定宽表达式保留在 IR 中。依赖分析使用保守上界处理它无法证明的条件和 bitmap。`mdrframe` 对当前能确认的作用域、使用阶段、静态循环和资源使用错误提供带位置的诊断。它没有通用的路径可满足性求解器。

