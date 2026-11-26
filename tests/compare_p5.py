"""Differential test for an external handwritten P5 project and mdrframe P5 RTL."""

import argparse
import random
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TRACE = re.compile(r"^TRACE (\d+) ([0-9a-f]{8}) ([0-9a-f]{8}) ([01])$", re.MULTILINE)
WRITE = re.compile(r"^\s*(\d+)@([0-9a-f]{8}):\s*(.*?)\s*<=\s*([0-9a-f]{8})\s*$", re.MULTILINE)
TESTBENCH = """module lockstep_tb;
    reg clk = 0;
    reg reset = 1;
    integer cycle = 0;
    mips uut(clk, reset);
    always #1 clk = ~clk;
    initial begin
        #6 reset = 0;
    end
    always @(negedge clk) begin
        if (!reset) begin
            $display("TRACE %0d %08h %08h %b", cycle, uut.pc, uut.D_IR, uut.stall);
            cycle = cycle + 1;
            if (cycle == 420) $finish;
        end
    end
endmodule
"""


def command(arguments, directory):
    return subprocess.run(arguments, cwd=directory, capture_output=True, text=True, timeout=35)


def require(result, action):
    if result.returncode:
        raise RuntimeError(f"{action} failed ({result.returncode})\n{result.stdout}{result.stderr}")


def compile_project(directory, iverilog, files):
    image = directory / "lockstep.vvp"
    image.unlink(missing_ok=True)
    (directory / "lockstep_tb.v").write_text(TESTBENCH, encoding="ascii")
    result = command([iverilog, "-g2005", "-I", ".", "-s", "lockstep_tb",
                      "-o", image.name, *files, "lockstep_tb.v"], directory)
    # The local Windows Icarus exits this way after successfully writing a vvp image.
    if result.returncode in (-1073741819, 0xC0000005) and image.is_file() and not result.stderr:
        return
    require(result, f"compile {directory.name}")


def i_type(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xffff)


def r_type(fn, rs, rt, rd, shift=0):
    return (rs << 21) | (rt << 16) | (rd << 11) | (shift << 6) | fn


def directed():
    # Use every base instruction. Keep control-transfer targets forward and put
    # ordinary instructions in delay slots.
    return [
        i_type(13, 0, 1, 17), i_type(43, 0, 1, 0),
        i_type(35, 0, 2, 0), i_type(43, 0, 2, 4),  # lw -> sw data: no stall
        i_type(35, 0, 3, 4), r_type(32, 3, 1, 4),  # lw -> add: one stall
        i_type(35, 0, 5, 4), i_type(4, 5, 1, 2),   # lw -> beq: two stalls
        i_type(8, 0, 6, -1), i_type(13, 0, 7, 0xbad), r_type(34, 4, 1, 6),
        r_type(37, 4, 6, 7), r_type(0, 0, 7, 8, 1), r_type(4, 1, 8, 9),
        i_type(35, 0, 0, 0), i_type(5, 0, 0, 2),    # $0 never stalls
        i_type(15, 0, 10, 0x1234),
        (2 << 26) | (0xc00 + 20), i_type(13, 0, 11, 0x111),
        i_type(13, 0, 11, 0xbad),
        i_type(13, 0, 12, 0x3000 + 26 * 4), i_type(43, 0, 12, 8),
        i_type(35, 0, 13, 8), r_type(8, 13, 0, 0),  # lw -> jr: two stalls
        i_type(13, 0, 20, 0x222), i_type(13, 0, 20, 0xbad),
        i_type(13, 0, 13, 0x3000 + 30 * 4), r_type(9, 13, 0, 14),
        r_type(37, 14, 0, 21), i_type(13, 0, 21, 0xbad),
        (3 << 26) | (0xc00 + 33), r_type(37, 31, 0, 22),
        i_type(13, 0, 22, 0xbad), i_type(13, 0, 22, 0x333),
        i_type(35, 0, 2, 0), i_type(13, 0, 2, 99),  # closest writer takes priority
        i_type(4, 2, 1, 2), i_type(13, 0, 16, 0xf00),
    ]


def random_program(seed):
    rng = random.Random(seed)
    words = []
    for _ in range(180):
        rs, rt, rd = (rng.randrange(9) for _ in range(3))
        word = rng.randrange(8) * 4
        kind = rng.randrange(10)
        if kind < 3:
            words.append(i_type(35, 0, rt, word))
        elif kind < 5:
            words.append(i_type(43, 0, rt, word))
        elif kind < 7:
            words.append(r_type(rng.choice((32, 34, 37, 4)), rs, rt, rd))
        elif kind == 7:
            words.append(i_type(8, rs, rt, rng.randrange(-128, 128)))
        elif kind == 8:
            words.append(i_type(13, rs, rt, rng.randrange(256)))
        else:
            words.append(r_type(0, 0, rt, rd, rng.randrange(8)))
    return words


def image(words):
    return "".join(f"{word:08x}\n" for word in words + [0] * (4096 - len(words)))


def read_smoke():
    return [int(line, 16) for line in (ROOT / "tests/p5_smoke.hex").read_text().splitlines()
            if line and not line.startswith("@")]


def simulate(directory, vvp, code, name):
    (directory / "code.txt").write_text(code, encoding="ascii")
    result = command([vvp, "lockstep.vvp"], directory)
    require(result, f"simulate {directory.name}/{name}")
    (directory / f"{name}.log").write_text(result.stdout, encoding="ascii")
    trace = TRACE.findall(result.stdout)
    if len(trace) != 420:
        raise RuntimeError(f"{directory.name}/{name}: expected 420 known trace samples, got {len(trace)}")
    # Verilog's ordering of same-edge RF and DM print calls is unspecified.
    return trace, sorted(WRITE.findall(result.stdout))


def compare(args):
    work = args.out.resolve()
    generated = work / "generated"
    reference = work / "reference"
    generated.mkdir(parents=True, exist_ok=True)
    reference.mkdir(parents=True, exist_ok=True)

    result = command([str(args.mdrframe.resolve()), "-o", str(generated),
                      str(ROOT / "examples/p5.mdr")], ROOT)
    require(result, "generate P5 RTL")
    source_files = sorted(args.p5.glob("*.v"))
    if not source_files:
        raise ValueError(f"No Verilog source files in {args.p5}")
    for path in [*source_files, *args.p5.glob("*.vh")]:
        shutil.copyfile(path, reference / path.name)
    files = (generated / "files.f").read_text(encoding="ascii").splitlines()
    compile_project(generated, args.iverilog, files)
    compile_project(reference, args.iverilog, [path.name for path in source_files])

    fixtures = [("smoke", read_smoke()), ("directed", directed())]
    fixtures += [(f"seed{seed}", random_program(seed)) for seed in range(16)]
    stalls = writes = 0
    for name, words in fixtures:
        code = image(words)
        expected, expected_writes = simulate(reference, args.vvp, code, name)
        actual, actual_writes = simulate(generated, args.vvp, code, name)
        for left, right in zip(expected, actual):
            if left != right:
                raise RuntimeError(f"{name}: cycle/PC/D_IR/stall differ\nreference {left}\ngenerated {right}")
        if expected_writes != actual_writes:
            raise RuntimeError(f"{name}: architectural writes differ; see {reference} and {generated}")
        stalls += sum(row[-1] == "1" for row in actual)
        writes += len(actual_writes)
    print(f"{len(fixtures)} programs; {len(fixtures) * 420} matching cycles; "
          f"{stalls} matching stalls; {writes} matching writes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--p5", type=Path, required=True, help="handwritten P5 source directory")
    parser.add_argument("--mdrframe", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=ROOT / "build/p5-diff")
    parser.add_argument("--iverilog", default="iverilog")
    parser.add_argument("--vvp", default="vvp")
    compare(parser.parse_args())


if __name__ == "__main__":
    main()

