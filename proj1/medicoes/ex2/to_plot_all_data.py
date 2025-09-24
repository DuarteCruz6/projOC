import os
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt

script_dir = os.path.dirname(os.path.abspath(__file__))
excel_path = os.path.join(script_dir, "all_data.xlsx")

df = pd.read_excel(excel_path)

df["array size"] = pd.to_numeric(df["array size"])
df["stride"] = pd.to_numeric(df["stride"])
df["# mean access time (nanosec)"] = pd.to_numeric(df["# mean access time (nanosec)"])     

plt.figure(figsize=(12, 6))

for stride in df["stride"].unique():
    subset = df[df["stride"] == stride]
    plt.plot(
        subset["array size"],
        subset["# mean access time (nanosec)"],
        marker='o',
        label=f"stride {stride}",
    )
    
plt.title("# mean access time (nanosec) vs Stride")
plt.xlabel("Array size")
plt.ylabel("# mean access time (nanosec)")

x_ticks = [2**i for i in range(12,24)]
plt.xscale("log", base=2)
plt.xticks(x_ticks, [f"{2**i}" for i in range(12,24)])
plt.grid(True, which="both", linestyle="--", linewidth=0.5)
plt.legend()

plot_path = os.path.join(script_dir, "plot-allstrides.png")
plt.savefig(plot_path)
plt.show()
print(f"Plot saved as: {plot_path}")