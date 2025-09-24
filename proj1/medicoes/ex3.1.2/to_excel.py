import os
import pandas as pd

script_dir = os.path.dirname(os.path.abspath(__file__))
file_path = os.path.join(script_dir, "test.out")
output_path = os.path.join(script_dir, "dados2.xlsx")

if os.path.exists(output_path):
    os.remove(output_path)
    print(f"Deleted old file: {output_path}")

data = []
with open(file_path, "r") as file:
    for line in file:
        row = []
        allInfo = line.strip().split()
        for index in range(len(allInfo)):
            info = allInfo[index]
            #cache_size	stride	avg_misses avg_time
            if index == 0:
                #cache size
                row.append(float(info[11:]))
            if index == 1:
                #stride
                row.append(float(info[7:]))
            if index == 2:
                #avg_misses
                row.append(float(info[11:]))
            if index == 3:    
                #avg_time
                row.append(float(info[9:])) 
            
        data.append(row)
        
df = pd.DataFrame(data, columns=["cache size", "stride", "avg_misses", "avg_time"])
df.to_excel(output_path, index=False)
