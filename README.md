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

