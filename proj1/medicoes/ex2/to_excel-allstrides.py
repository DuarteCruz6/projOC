import os
import pandas as pd

script_dir = os.path.dirname(os.path.abspath(__file__))
file_path = os.path.join(script_dir, "spark.out")
output_path = os.path.join(script_dir, "all_data.xlsx")

if os.path.exists(output_path):
    os.remove(output_path)
    print(f"Deleted old file: {output_path}")

data = []
with open(file_path, "r") as file:
    firstLine = True
    for line in file:
        if firstLine:
            firstLine = False
        else:
            row = []
            info = line.strip().split()
            #size	stride	elapsed(s)	cycles	#accesses a[i]	 mean access time
            row.append(float(info[0]))
            row.append(float(info[1]))
            row.append(float(info[5]))
            data.append(row)
        
df = pd.DataFrame(data, columns=["array size", "stride", "# mean access time (nanosec)"])
df.to_excel(output_path, index=False)
