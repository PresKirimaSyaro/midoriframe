# midoriframe

midoriframe 把一份描述 MIPS 指令的 `.mdr` 文件编译成 BUAA CO P5 风格的五级流水线 Verilog 工程。用 DSL 写出每条指令的计算、访存、跳转和写回，编译器按 P5 的流水级生成控制器和流水寄存器，阻塞由独立的 `STALL` 模块判断。命令行工具名为 `mdrframe`。

## 快速开始

需要 CMake 3.20+ 和 C++20 编译器。

```sh
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

在 Windows 的 MinGW 环境可用 `cmake -S . -B build -G "MinGW Makefiles"`，可执行文件为 `build/mdrframe.exe`。多配置生成器把可执行文件放在相应的配置目录下。

从仓库根目录生成并运行示例。下面使用单配置构建的可执行文件路径，Windows 下可用 `.\build\mdrframe.exe` 执行第一条命令。

仿真可以使用 Icarus Verilog 提供的 `iverilog` 与 `vvp`。

```sh
./build/mdrframe -o build/p5 --program tests/p5_smoke.hex examples/p5.mdr
cd build/p5
iverilog -g2005 -s mips_tb -o sim.vvp -f files.f # 或者直接使用 iverilog -o sim *.v *.vh
vvp sim.vvp
```

默认在当前目录生成 `<输入文件名>.rtl/`，也可用 `-o` 指定目录。`--program` 把机器码镜像复制为 `code.txt`。省略时沿用输出目录已有的镜像，新目录则填充零指令。从生成目录运行仿真，IM 在那里读取 `code.txt`，testbench 在那里写入 `wave.vcd`。

## 语法速览

这条 `lw` 在 D 公布写回目标，在 E 计算地址，在 M 读取数据。编译器会自动解析出 `GPR[base]` 的使用时间、写入 `GPR[rt]` 的值的产生时间，并自动生成阻塞、转发逻辑。

```mdr
lw {
    match(ins[31:26] == 6'h23);
    base: 5 = ins[25:21];
    rt: 5 = ins[20:16];
    off: 16 = ins[15:0];

    D {
        update 32'd1 << rt;
    }

    addr: 32;
    E {
        addr = grf_read(base) + {{16{off[15]}}, off};
    }
    M {
        yield dm_read(addr, 4);
    }
}
```

| 写法 | 含义 |
|---|---|
| `match(条件表达式);` | 每条指令恰有一个，表示指令码 `ins` 被解释成当前指令的条件 |
| `name: 位宽;` 或 `name: 位宽 = 表达式;` | 声明定宽变量。外层声明可放在流水级之间，对后续流水级可见 |
| `D {...}`、`E {...}`、`M {...}` | 表明每一流水级的处理逻辑，不用的级可以省略 |
| `if / else`、`for` | 主体可写一条语句或大括号块。`for` 在编译期展开，支持倒序和跳步 |
| `grf_read(r)` | `GPR[r]` 寄存器的值 |
| `dm_read(addr, size)` | 必须位于 M 级。读取 DM 在 `addr[31:2]` 地址处 `size` 个字节。目前 p5 只支持 `size = 4` 整字读取 |
| `dm_write(addr, val)` | 必须位于 M 级。向 DM 的 addr[31:2] 位置写入32 位 val |
| `jump(addr)` | 必须位于 D 级。进行跳转，在延迟槽后跳转到 `addr` |
| `annul_delay_slot()` | 必须位于 D 级。清空延迟槽 |
| `update bitmap` | 指定可能的 GPR 写入目标。首次更新设定目标集合，后续取交集 |
| `yield value` | 产生写入 GPR 目标的值 |

完整语法、控制时序和练习题的 RTL 对照见 [SPEC.md](SPEC.md)。

## 检查生成结果

| 命令选项 | 输出 |
|---|---|
| `--emit=verilog` | 默认模式，生成 P5 风格的 Verilog 工程和 `files.f` |
| `--emit=ir` | 带位宽、流水级和执行条件的中间表示 |
| `--emit=deps` | GPR 使用级、跨级传递和 Promise 路径等依赖信息 |

- [P5 基础指令集](examples/p5.mdr)包含可直接生成工程的 16 条指令。
- [动态目的寄存器](examples/lrm.mdr)展示如何根据内存读数收缩写回目标。
- [条件就绪时序](examples/conditional.mdr)展示一条指令可在不同流水边界转发。

CMake 找到 Icarus Verilog 时，CTest 会运行生成工程的仿真测试。也可以把本地手写 P5 工程交给对拍脚本。它比较每拍的取指 PC、D 级指令、阻塞信号，以及寄存器和内存写入。

```sh
python tests/compare_p5.py --mdrframe build/mdrframe --p5 /path/to/handwritten-p5
```

## 练习题示例

下面把 P5 题目的行为 RTL 和对应的 MDR 放在一起。RTL 里的 `GRF` 表示旧寄存器值，`DM` 按字索引，`PC` 是本条指令地址，`next_pc` 是延迟槽后的取指地址。每段 MDR 都可以保存成单独的 `.mdr` 文件，也可以加到 [P5 基础指令集](examples/p5.mdr) 中。更多题例见 [SPEC.md](SPEC.md)。

### P5-Q02 · bgezall

编码为 `110000 rs 00000 offset16`。总是将 `PC+8` 写入 `$31`。`rs` 为非负有符号数时保留延迟槽并跳转；否则清槽。

**题面 RTL**

```verilog
GRF[31] <= PC + 32'd8;
if ($signed(GRF[rs]) >= 0)
    next_pc = PC + 32'd4 + {{14{offset[15]}}, offset, 2'b00};
else begin
    annul_slot = 1'b1;
    next_pc = PC + 32'd8;
end
```

**对应的 MDR**

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

### P5-Q20 · ccc

编码为 `000000 rs rt rd 00000 110101`。比较 `rs` 的前导零数与 `rt` 的前导一数。前者严格更大时取 `rs` 的高半字与 `rt` 的低半字，否则交换来源。

**题面 RTL**

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

**对应的 MDR**

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

**题面 RTL**

```verilog
address = GRF[base] + {{14{offset[15]}}, offset, 2'b00};
word = DM[address[31:2]];
mixed = word ^ GRF[rt];
if (mixed[31]) GRF[rt] <= word;
else GRF[mixed[3:0]] <= word;
```

**对应的 MDR**

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
