# mdrframe

`mdrframe` 是五级顺序流水线指令 DSL 的 C++20 编译器。它读取单个 `.mdr` 指令集，可输出带阶段的中间表示、依赖表或 P5 Verilog 工程。

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

在其他环境可以省略 `-G` 或选择本机 CMake 生成器。默认在当前目录生成 `<输入文件名>.rtl/`，例如 `p5.rtl/`。`--emit=ir` 输出完整 IR，`--emit=deps` 输出依赖表，默认写到标准输出。若输入名没有扩展名且路径不存在，程序尝试附加 `.mdr`。显式提供路径时也可以读取先前的 `.dsl` 样例。

P5 默认使用二参数整字写 `dm_write(addr, val)`，整字访存忽略地址低两位。若要分析早期 P6 样例中的字节或半字写，显式加 `--p6-narrow-writes`。

## 生成和仿真 P5 工程

```powershell
.\build\mdrframe.exe -o build\p5 --program tests\p5_smoke.hex examples\p5.mdr
Push-Location build\p5
iverilog -g2005 -s mips_tb -o sim.vvp -f files.f
vvp sim.vvp
Pop-Location
```

`-o` 指定工程目录。`mips.v` 连接取指、`FDreg`、`DEreg`、`EMreg`、`MWreg` 与 `Controller`。`Controller.v` 在 D 从 IR 译码，生成 `ins_add`、`ins_sub` 等单比特控制标志，再用各流水级的指令标志选择计算与副作用。`rs`、`rt`、`rd` 等编码字段、RF 读口、转发及阻塞逻辑均由同一数据通路共用。跨级计算结果按使用级分配寄存器，D→E 和 E→M 各自按指令中所需的最大宽度复用存储。工程还包含 IM、DM、RF、Promise 定义、原 P5 的 `mips_tb.v` 和仿真文件清单。

IM 有 4096 个 32 位字，起始地址为 `0x3000`。`--program` 将十六进制程序镜像复制为 `code.txt`。省略该选项时保留已有的 `code.txt`，首次生成则填充零指令。在生成目录运行仿真时，IM 从该目录读取 `code.txt`，testbench 在同一目录保存 `wave.vcd`。

DM 有 3072 个 32 位字，复位清零，支持整字写及整字、半字、字节读。`--p6-narrow-writes` 可用于 IR 和依赖分析，P5 Verilog 后端会拒绝窄写。跳转与清槽只在 D 级执行，DM 只在 M 级读写。寄存器写回和整字存储沿用 P5 日志格式。CMake 找到 `iverilog` 和 `vvp` 时，CTest 同时运行原 testbench 下的基础流水线和动态 Promise 回归。

### 与手写 P5 逐拍对照

```powershell
python tests/compare_p5.py --mdrframe build/mdrframe.exe --p5 D:/Dcode/co/p5
```

脚本给两个实现加载同一份指令镜像，复位后逐拍比较取指 PC、D 级指令和 `stall`，并比较同拍的寄存器与内存写入。脚本使用独立监视 testbench，原工程的文件直接复制到测试目录。日志保存于 `build/p5-diff/`。

## 控制流语法

`if`、`else` 和 `for` 的主体可以写成一条语句，也可以用大括号包含多条语句。单条语句仍有自己的作用域。嵌套 `if` 中，`else` 与最近的尚未匹配的 `if` 绑定。D、E、M 流水级必须使用大括号。

静态 `for` 支持 `<`、`<=`、`>`、`>=` 比较，以及 `i = i + step`、`i = i - step` 两种更新。起点、限值和正步长须在编译时确定。例如 `for (i = 31; i >= 0; i = i - 1)` 从 31 到 0 展开，`for (i = 7; i >= 0; i = i - 2)` 只访问 7、5、3、1。

指令外层声明可以写在流水级之前或流水级之间。声明对后续流水级可见，例如在 D 和 E 之间声明的 `addr` 可在 E 中赋值，再由 M 读取。

## 输出约定

- `priority` 按源文件顺序编号，首个命中者生效。译码未命中时无副作用。
- `values` 是带宽度、符号、计算级和操作数的表达式图。`fit` 标记 Verilog 表达式的赋值位宽上下文，保留源运算符和定宽边界。算术节点上的宽度是它自身的推导宽度，后端仍需按使用处的 Verilog 上下文延展计算。静态循环已展开，`phi` 合并分支值，`insert` 更新位或切片。一个 IR 值编号只定义一次。
- `effects` 给出带执行条件的有序动作，包括访存、跳转、清槽和 Promise 更新。`%N` 引用值，`#N` 引用动作。内存及 GPR 写回的数据和目的可由不同阶段产生。
- `register_reads` 给出 D 级可计算的读号、读使能和实际使用级。D/E/M 的 `Tuse` 分别为 0/1/2。只依赖编码或 PC 的条件可用于 D 级读口选择，依赖 GPR 或 DM 的条件保守处理。读口上界按整份指令集推导，可合并等价的读号，`$0` 可直接取零。
- `pipeline_carries` 列出 D→E、E→M 之间需要传递的值。普通值已完成计算。标为 `rf-address+candidate` 的操作数还要随级接受旁路更新。IR 中的 `ins` 和 `pc` 由后端作为每条流水指令的公共信息提供。
- `promise` 给出初始 RAW 候选上界及各路径的 `update`、`yield` 和保证就绪的边界。首次 `update` 赋值，后续取交集。零 bitmap 无写回。路径未定时取剩余可能路径中最迟的可用边界，已确定时按该路径判断。未枚举的复杂情况仍有有序受保护动作可供后端分析。`ready_by` 按有效的单目标写回程序计算；最终多位的行为依照 DSL 规范未定义。

`examples/p5.mdr` 包含 16 条 P5 基础指令。`examples/lrm.mdr` 由内存数据决定写回哪一个 GPR。`examples/conditional.mdr` 展示同一条指令在 E→M 或 M→W 就绪的两条路径。

编译期求值支持最多 64 位的常量计算，较宽的定宽表达式保留在 IR 中。依赖分析使用保守上界处理它无法证明的条件和 bitmap。`mdrframe` 对当前能确认的作用域、使用阶段、静态循环和资源使用错误提供带位置的诊断。它没有通用的路径可满足性求解器。

