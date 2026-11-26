# midoriframe DSL v0.1 语法教程

本文以当前 `mdrframe` 实现为准。安装和仿真命令见 [README](README.md)。

## 1. 写第一条指令

一个 `.mdr` 文件描述一套指令， 每一条指令的代码是下面这个样子：

```mdr
add {
	// 这是行注释
    match(ins[31:26] == 6'd0 && ins[5:0] == 6'h20);
    rs: 5 = ins[25:21];
    rt: 5 = ins[20:16];
    rd: 5 = ins[15:11];
    D { update 32'd1 << rd; } // D 级确定写入目标为 rd
    E { yield grf_read(rs) + grf_read(rt); } // E 级产生要写入的值是 GPR[rs] + GPR[rt]
    /*
    	这是跨行注释
    */
}
```

每个指令定义包含唯一的助记符（`add`）和一个 `match`，`match(条件)` 表示指令编码被解释为当前指令的条件。

`D{...}` 和 `E{...}` 表示这条指令在 D, E 级的执行逻辑。M 级 `add` 什么都不做，因此 `M{...}` 可以省略，F 级和 W 级由编译器生成，代码中不需要出现。

`update 32'd1 << rd` 是用来指定这条指令写入的寄存器目标，`yield 表达式` 用来指定写入目标寄存器的值。这两个部分的细节会在后面详细介绍。

`ins` 是当前指令的 32 位机器码。`match` 只允许引用机器码 `ins` 和编译期常量，表达式宽度必须是 1 位。字段别名若在 `match` 中使用，须先声明，例如像下面这样：

```mdr
add {
	opcode: 6 = ins[31-:6];
	funct: 6 = ins[0+:6];
    match(opcode == 6'd0 && funct == 6'h20);
    rs: 5 = ins[25:21];
    rt: 5 = ins[20:16];
    rd: 5 = ins[15:11];
    D { update 32'd1 << rd; }
    E { yield grf_read(rs) + grf_read(rt); }
}
```

`ins` 与 `pc` 在各指令内部始终表示同一条指令。`pc` 是该指令的字节地址，供 D、E、M 读取。D、E、M 块可省略，每级最多出现一次，出现时按 D、E、M 排列，且必须有大括号。F 负责取指，W 负责提交 GPR 写回，两级由生成器提供。

## 2. 数据、位宽和作用域

### 声明与赋值

声明一个定位宽向量：`name: width;` 

声明的同时可以初始化：`name: width = expr;`

向量的二进制位下标为 `0` 到 `width - 1`，相当于 verilog 中的 `reg [width - 1:0] name;`

位宽必须是编译期正整数，没有初始值的变量需要先赋值再读；在条件赋值中，应保证所有能到达该读取的路径都已经定义它。只有部分位已定义时，只能读取已知定义的位或位段。

普通的赋值语义和 verilog 阻塞赋值类似，例如：

```mdr
...
D {
	tmp: 5 = rs;
	rs = rt;
	rt = tmp;
}
```

这样会交换 `rs` 和 `rt` 两个向量。

每个向量有自己的作用域，和 C 语言类似。例如上面的例子中，`tmp` 的作用域只在 D 级中，在 D 级大括号外无法访问。

指令外层声明可以出现在级块之间，并从声明处向后可见。打算跨级传递的值应在指令外层声明。外层带初值的声明是只读译码别名，初值只能依赖 `ins` 和常量；需要在流水级内修改的变量应在外层只声明位宽。块内变量只在所属块及其子块可见。省略大括号的控制流主体仍有自己的作用域。

```mdr
beq {
    match((ins & 32'hfc000000) == 32'h10000000);
    rs: 5 = ins[25:21];
    rt: 5 = ins[20:16];
    imm: 16 = ins[15:0];

    D {
        if (grf_read(rs) == grf_read(rt)) {
        	off: 32 = {{14{imm[15]}}, imm, 2'b00};
            jump(pc + 32'd4 + off);
        }
    }
}
```

例如 `off` 的作用域就只在 `if` 中，在 `if` 外无法访问，这里和 C 语言是一样的。

### 字面量与符号性

支持十进制整数，以及 Verilog 的二进制、八进制、十进制、十六进制定宽字面量，如 `5'd31`、`16'hff00`、`6'b11_0111`。下划线用于分组。`32'sd4` 是带符号字面量。

带进制标记的字面量也可省略宽度，如 `'hff`。Verilog 未知位可用 `x` 或 `z` 表示，须编译期求出确定整数的地方不能使用未知位。

变量默认无符号。`signed(expr)` 保持位宽并把表达式解释为有符号值。两边都应按有符号比较时，可写 `signed(a) < signed(b)`。赋值、比较和运算按 Verilog 的位宽及符号上下文处理，窄目标会截断。

可以通过显式拼接避免混合符号表达式中的扩展歧义，例如 `{{16{imm[15]}}, imm}` 把 16 位立即数按符号扩展到 32 位。

### 运算、选择和拼接

这里涵盖了 verilog 支持的大部分运算方式。

| 类别 | 写法 |
|---|---|
| 算术 | `+ - * / % **`，以及一元 `+ -` |
| 按位 | `~ & \| ^ ~^ ^~` |
| 一元归约 | `& ~& \| ~\| ^ ~^ ^~` |
| 逻辑 | `! && \|\|` |
| 比较 | `< <= > >= == != === !==` |
| 移位 | `<< >> <<< >>>` |
| 条件表达式 | `condition ? a : b` |
| 位选与位段 | `x[i]`、`x[31:16]`、`x[i +: 8]`、`x[i -: 8]` |
| 拼接与重复 | `{a, b}`、`{4{byte}}` |
| 符号转换 | `signed(expr)` |

从高到低的主要优先级是选择和一元运算、乘方、乘除余、加减、移位、关系、相等、按位 AND/XOR/OR、逻辑 AND/OR、三目。用圆括号显式标明需要的分组。常规位段的两个端点须是编译期常量且高位在前。`+:` 和 `-:` 的起点可变但宽度必须已知。重复次数须是编译期正整数。

## 3. 条件和静态循环

`if (cond) statement` 可带 `else statement`，两个分支各自可写一条语句或用大括号写多条。`else if` 是 `else` 后接一个 `if`，嵌套时 `else` 匹配最近一个未匹配的 `if`。

```text
if (flag)
    value = a;
else
    value = b;

for (i = 31; i >= 0; i = i - 1) // i 是循环索引，无需声明
    count = count + word[i];
```

`for` 的条件只能是后面的四种： `i < limit`、`i <= limit`、`i > limit` 、`i >= limit`

更新只可以使用 `i = i + step` 或 `i = i - step`两种方式。

这是为了保证循环的初值、边界和正步长都在编译期求出，索引是循环体内只读的有符号 32 位值。

循环可以正向、倒向和跳步，也可嵌套。内层边界可引用外层已展开的索引。

例如 `for (i = 7; i >= 0; i = i - 2)` 访问 `7、5、3、1`。条件开始即为假时不展开主体。编译器会拒绝零步长、越界以及使成立条件远离终点的更新。一条指令的展开次数上限为 65,536。

## 4. 流水线原语

| 原语 | 可用阶段 | 语义 |
|---|---|---|
| `grf_read(r)` | D/E/M | 返回指定 GPR 的旧值，按使用级等待或转发；`$0` 恒为零 |
| `dm_read(addr, size)` | M | 按字节地址读取 `size` 字节，返回相应宽度的无符号值；`size` 必须为常量 1、2 或 4 |
| `dm_write(addr, val);` | M | 写一个 32 位整字；忽略地址低两位 |
| `jump(addr);` | D | 把下一次取指重定向到 `addr`，已取出的顺序指令仍是延迟槽 |
| `annul_delay_slot();` | D | 取消延迟槽；若同拍没有 `jump`，从当前指令 `pc + 8` 继续 |
| `update bitmap;`、`yield value;` | D/E/M | 更新当前指令的写回承诺 |

GPR 编号为 `0 ~ 31`，用五位整数指定。

每条执行路径最多一个 DM 访问、一个跳转、一次清槽。

DM 读写控制和地址不能依赖同一次 DM 读数。P5 内置 DM 为小端，字节由低两位选取、半字由 bit 1 选取、整字忽略低两位。需要符号扩展窄读取时，可用 `yield signed(data);`。

`--p6-narrow-writes` 仅用于 IR 与依赖分析，允许旧式 `dm_write(addr, size, val);`，其中 `size` 为 1 或 2。P5 Verilog 后端只接受整字写入。

## 5. Promise、阻塞和转发

每条指令最多写一个 GPR。`update` 的操作数按 32 位解释，每一位代表对应编号的寄存器。第一次 `update` 设定 bitmap，后续 `update` 与当前 bitmap 取交集。`yield` 单独提供 32 位写回值，可以出现在确定最终目标之前或之后，每条路径至多一次。W 只有在目标唯一且值已产生时才提交；无更新、空 bitmap 或仅选 `$0` 时没有架构写入。最终多位或选中非零寄存器却没有 `yield` 的行为未定义。

例如 `update 32'h0000_ffff;` 表示将来可能写低 16 个寄存器。若最后实际要写 `rt`，而 `rt` 可能大于 15，就须一开始用 `(32'd1 << rt) | 32'h0000_ffff` 把它纳入候选。被排除的位不能被后续更新重新加入。`update 32'd0;` 可以取消尚未确定的写回。

GPR 在 D 建立读请求，D、E、M 中 `grf_read` 的使用级分别对应 `Tuse=0、1、2`。例如，在 D 把 `grf_read(rt)` 读进局部量时，即使该量到 M 才传给存储，读取本身仍发生在 D；若希望使用 M 级旁路，应在 M 调用 `grf_read(rt)`。

D 检查 E、M、W 中离当前指令最近的候选写者。仅当它还需要的流水边界数超过 `Tuse` 才阻塞。若目标在后级才首次更新，后端会提前公布保守的 RAW 候选集合，避免消费者过早读取旧值。稳定的单一目标与写回数据都已具备时，下一拍可转发。实际路径已知时可使用该路径的完成边界；路径未定时保守等待。阻塞保持 PC 和 FD，在 DE 插入气泡，较老指令继续前进。

以基础 P5 指令为例，紧邻的 `lw → add` 在 E 消费 load 结果，会停一拍；`lw → beq` 在 D 消费，会停两拍；`lw → sw` 如果仅存储数据相关，则可以在 M 接收转发而无需停拍。[条件就绪示例](examples/conditional.mdr)展示同一条指令可能在 E→M 或 M→W 后才可转发。

## 6. 练习题与题面 RTL 对照

题目编号来自 *BUAA-CO_P3-P6-local_judge* 的《P5 往年题目册》和《P5 模拟题目册》。RTL 栏是对应题意的行为片段，`GRF` 表示指令执行前的寄存器值，`DM` 按字索引，`PC` 表示本条指令地址，`next_pc` 表示延迟槽后的控制流目标。片段用于说明题意，单独一段并不包含流水线和阻塞处理。下面每个 `mdr` 代码块都是一条完整定义，可单独保存，也可与 [基础指令集](examples/p5.mdr)放入同一个输入文件。

### P5-Q02 · bgezall

编码为 `110000 rs 00000 offset16`。总是将 `PC+8` 写入 `$31`。`rs` 为非负有符号数时保留延迟槽并跳转；否则清槽。

```verilog
GRF[31] <= PC + 32'd8;
if ($signed(GRF[rs]) >= 0)
    next_pc = PC + 32'd4 + {{14{offset[15]}}, offset, 2'b00};
else begin
    annul_slot = 1'b1;
    next_pc = PC + 32'd8;
end
```

```mdr
bgezall {
    match(ins[31:26] == 6'b110000 && ins[20:16] == 5'd0);
    rs: 5 = ins[25:21];
    off: 16 = ins[15:0];
    D {
        update 32'd1 << 5'd31;
        yield pc + 32'd8;
        if (signed(grf_read(rs)) >= 32'sd0)
            jump(pc + 32'd4 + {{14{off[15]}}, off, 2'b00});
        else
            annul_delay_slot();
    }
}
```

### P5-Q06 · lrm

编码为 `110011 base rt offset16`。读一个字，以最低五位为目的编号，把旧 `rt` 写到该寄存器。

```verilog
address = GRF[base] + {{16{offset[15]}}, offset};
word = DM[address[31:2]];
GRF[word[4:0]] <= GRF[rt];
```

```mdr
lrm {
    match(ins[31:26] == 6'b110011);
    base: 5 = ins[25:21];
    rt: 5 = ins[20:16];
    off: 16 = ins[15:0];
    D { update 32'hffff_ffff; }
    addr: 32;
    E { addr = grf_read(base) + {{16{off[15]}}, off}; }
    M {
        word: 32 = dm_read(addr, 4);
        yield grf_read(rt);
        update 32'd1 << word[4:0];
    }
}
```

### P5-Q20 · ccc

编码为 `000000 rs rt rd 00000 110101`。比较 `rs` 的前导零数与 `rt` 的前导一数。前者严格更大时取 `rs` 的高半字与 `rt` 的低半字，否则交换来源。

```verilog
zeros = 0; ones = 0; stop_rs = 0; stop_rt = 0;
for (i = 31; i >= 0; i = i - 1) begin
    if (!stop_rs && !a[i]) zeros = zeros + 1;
    else stop_rs = 1;
    if (!stop_rt && b[i]) ones = ones + 1;
    else stop_rt = 1;
end
GRF[rd] <= zeros > ones ? {a[31:16], b[15:0]} : {b[31:16], a[15:0]};
```

这里 `a = GRF[rs]`，`b = GRF[rt]`。计数最大为 32，使用 6 位即可。

```mdr
ccc {
    match(ins[31-:6] == 6'd0 && ins[10:6] == 5'd0 && ins[0+:6] == 6'h35);
    rs: 5 = ins[25:21];
    rt: 5 = ins[20:16];
    rd: 5 = ins[15:11];
    D { update 32'd1 << rd; }
    E {
        a: 32 = grf_read(rs);
        b: 32 = grf_read(rt);
        zeros: 6 = 0;
        ones: 6 = 0;
        stop_rs: 1 = 0;
        stop_rt: 1 = 0;
        for (i = 31; i >= 0; i = i - 1) {
            if (!stop_rs && !a[i]) zeros = zeros + 1;
            else stop_rs = 1;
            if (!stop_rt && b[i]) ones = ones + 1;
            else stop_rt = 1;
        }
        yield zeros > ones ? {a[31:16], b[15:0]} : {b[31:16], a[15:0]};
    }
}
```

### P5-M10 · lwxsel

编码为 `110111 base rt offset16`。偏移量以字为单位。把读出的字与旧 `rt` 异或。如果最高位为 1 就写 `rt`，否则用异或结果的低四位决定目的寄存器。两条路径都写原始读数。

```verilog
address = GRF[base] + {{14{offset[15]}}, offset, 2'b00};
word = DM[address[31:2]];
mixed = word ^ GRF[rt];
if (mixed[31]) GRF[rt] <= word;
else GRF[mixed[3:0]] <= word;
```

```mdr
lwxsel {
    match(ins[31-:6] == 6'b11_0111);
    base: 5 = ins[25:21];
    rt: 5 = ins[20:16];
    off: 16 = ins[15:0];
    D { update (32'd1 << rt) | 32'h0000_ffff; }

    addr: 32;
    E { addr = grf_read(base) + {{14{off[15]}}, off, 2'b00}; }
    M {
        word: 32 = dm_read(addr, 4);
        mixed: 32 = word ^ grf_read(rt);
        yield word;
        if (mixed[31]) update 32'd1 << rt;
        else update 32'd1 << mixed[3:0];
    }
}
```

## 7. 阅读 IR 和当前实现边界

用 `--emit=ir` 查看带位宽、符号、阶段的表达式图。`%N` 表示值编号，`#N` 表示有序副作用，`fit` 标记赋值的位宽上下文。用 `--emit=deps` 查看译码优先级、GPR 读号、`Tuse`、跨级值、候选 bitmap 及 Promise 的 `ready_by`。仅依据 PC 或指令编码的读条件可以提前选择读口；依赖 GPR 或内存的条件采用保守上界。

P5 工程的顶层为 `mips(input clk, reset)`，使用 `FDreg`、`DEreg`、`EMreg`、`MWreg` 连接各级。`DCtrl`、`ECtrl`、`MCtrl` 各自读取本级 IR，用 `ins_add` 这样的单比特信号选择控制逻辑。同一级中等价的指令切片和控制表达式只生成一次。`DCtrl` 提供各使用级的 GPR 请求，`STALL` 根据请求和较老指令的 Promise 决定是否停拍。工程带 IM、DM、RF 和原 P5 testbench。IM 从 `0x3000` 开始，容量 4,096 字；DM 有 3,072 字。复位清零 DM 和 GPR。已计算的普通跨级值按照使用级传递，尚未消费的 GPR 操作数还可接受后续旁路。

当前编译器每次读取一个输入文件。语言范围是定宽组合逻辑与 P5 指令级副作用，包含静态循环，暂未提供用户函数、数组、`case`、`while`、多周期 MDU、HI/LO、中断、异常或用户自定义的跨指令状态。向量宽度上限为 1,048,576 位，编译期常量计算支持至多 64 位；更宽的定宽计算可以保留到 Verilog 后端。运行期路径和目标唯一性无法完全静态证明时，需要由作者保证上述约束。

