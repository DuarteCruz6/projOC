import os
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np

script_dir = os.path.dirname(os.path.abspath(__file__))
excel_path = os.path.join(script_dir, "dados.xlsx")

df = pd.read_excel(excel_path)

df["array size"] = pd.to_numeric(df["array size"])
df["stride"] = pd.to_numeric(df["stride"])
df["# mean access time (nanosec)"] = pd.to_numeric(df["# mean access time (nanosec)"])

plt.figure(figsize=(12, 6))

for array in df["array size"].unique():
    subset = df[df["array size"] == array]
    plt.plot(subset["stride"], subset["# mean access time (nanosec)"], marker='o', label=f"array size {array}")

plt.title("# mean access time (nanosec) vs Stride")
plt.xlabel("Stride")
plt.ylabel("# mean access time (nanosec)")


# Customize X-axis: 2^0 → 2^20 with steps 2^x
x_ticks = [2**i for i in range(21)]
plt.xscale("log", base=2)
plt.xticks(x_ticks, [f"2^{i}" for i in range(21)])

plt.grid(True, which="both", linestyle="--", linewidth=0.5)
plt.legend()

plot_path = os.path.join(script_dir, "plot.png")
plt.savefig(plot_path)
plt.show()

print(f"Plot saved as: {plot_path}")
