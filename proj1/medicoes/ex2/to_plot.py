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

# Create a separate plot for each stride value
for stride in df["stride"].unique():
    subset = df[df["stride"] == stride]
    
    plt.figure(figsize=(12, 6))
    plt.plot(subset["array size"], subset["# mean access time (nanosec)"], marker='o')
    
    plt.title(f"# mean access time (nanosec) vs Array Size (stride = {stride})")
    plt.xlabel("Array Size")
    plt.ylabel("# mean access time (nanosec)")
    
    # Use logarithmic scale for X-axis if array sizes grow exponentially
    plt.xscale("log", base=2)
    plt.xticks([2**i for i in range(int(np.log2(subset["array size"].min())), 
                                    int(np.log2(subset["array size"].max())) + 1)],
               [f"2^{i}" for i in range(int(np.log2(subset["array size"].min())), 
                                         int(np.log2(subset["array size"].max())) + 1)])
    
    plt.grid(True, which="both", linestyle="--", linewidth=0.5)
    
    plot_path = os.path.join(script_dir, f"plot_stride_{stride}.png")
    plt.savefig(plot_path)
    #plt.show()
    
    print(f"Plot for stride {stride} saved as: {plot_path}")
