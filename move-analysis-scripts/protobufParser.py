# %%
import pandas as pd
import subprocess
import re
import sys
from concurrent.futures import ProcessPoolExecutor

# %%
ITERATIONS=5

# %%
# for i in range(i):
def parse(moved: bool):
    path = "./moveless/protobuf/bazel-bin/benchmarks/benchmark"
    if (moved):
        path = "./move/protobuf/bazel-bin/benchmarks/benchmark"
    c = str(subprocess.check_output(path))
    out = c.split("----------------------------------------------------------------------------------------")

    # %%
    table_raw = out[len(out) - 1]
    parsed = re.sub(" +", " ", table_raw.replace(", ", "")).replace(" ns", "")

    # %%
    split = parsed.split("\\n")
    out = [[i for i in re.sub("bytes_per_second=.*", "", re.sub("items_per_second=.*", "", i)).split(" ") if i != ''] for i in split if len(i) > 1]

    # %%
    df = pd.DataFrame(out)
    df.columns = ["Benchmark", "Time", "CPU", "Iterations"]
    return df

def pct(move, moveless): 
    series = {"Benchmark": move["Benchmark"]}
    for i in range(1, len(moveless.columns) - 1):
        name = moveless.columns[i]
        ser1 = pd.to_numeric(moveless[name])
        ser2 = pd.to_numeric(move[name])
        series[name] = ((ser1 - ser2)/ser2) * 100
    return pd.DataFrame(series)

if (__name__ == "__main__"):
    if (len(sys.argv) > 1):
        ITERATIONS = int(sys.argv[1])
    tmp = pct(parse(True), parse(False))
    processes = []
    with ProcessPoolExecutor(8) as pool:
        for i in range(ITERATIONS - 1):
            processes.append(pool.submit(pct, parse(True), parse(False)))
    dfOut = [out.result() for out in processes]
    tmp["Time"] += sum([out["Time"] for out in dfOut])
    tmp["CPU"] += sum([out["CPU"] for out in dfOut])
    tmp["Time"] /= ITERATIONS
    tmp["CPU"] /= ITERATIONS
    print(tmp)