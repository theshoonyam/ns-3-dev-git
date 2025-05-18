import os
import re
import glob

import pandas as pd
import numpy as np
import sys
import statsmodels.api as sm
from statsmodels.formula.api import ols

def anova(loc):
    # 1. Define factor levels
    nodes=[2,4,8,16,32,64]
    rates=[10, 20, 50, 100, 200, 500]
    # 2. Collect data from files
    file_pattern = os.path.join(loc, "*.txt")
    records = []

    for filepath in glob.glob(file_pattern):
        print("here")
        filename = os.path.basename(filepath)
        # Match filenames like "x_y_z_0.txt"
        match = re.match(r'^(\d+)_(\d+)\.txt$', filename)
        if not match:
            continue  # skip any file that doesn't match the pattern

        x_idx = int(match.group(1))
        y_idx = int(match.group(2))

        # Read numeric data from file
        with open(filepath, 'r') as f:
            lines = f.readlines()
            delays = [line.split()[2] for line in lines if line.strip()]

        if not delays:
            # Skip if the file has no valid numeric lines
            continue

        # Compute mean of the delays
        mean_delay = delays[0]


        # Map indices to actual factor values
        x_val = nodes[x_idx]
        y_val = rates[y_idx]

        records.append({
            "nodes": x_val,
            "rate": y_val,
            "mean_delay": mean_delay
        })

    # If no records, nothing to analyze
    if not records:
        print(f"No valid data found in '{loc}' matching x_y_z.txt.")
        return

    # 3. Convert to DataFrame
    df = pd.DataFrame(records)

    # Clean data: remove any Inf/NaN in mean_delay
    df["mean_delay"] = pd.to_numeric(df["mean_delay"], errors="coerce")
    df.replace([np.inf, -np.inf], np.nan, inplace=True)
    df.dropna(subset=["mean_delay"], inplace=True)

    if df.empty:
        print("No valid rows left after removing Inf/NaN values.")
        return
    
    df["nodes"] = df["nodes"].astype("category")
    df["rate"] = df["rate"].astype("category")

    formula = "mean_delay ~ C(nodes) + C(rate)"

    model = ols(formula, data=df).fit()

    try:
        anova_table = sm.stats.anova_lm(model, typ=2)
        print(anova_table)
    except Exception as e:
        print("\n*** ANOVA Error ***")
        print(str(e))
        print("""
        This typically occurs if there are not enough replicates per factor 
        combination to estimate all interaction terms. If you have exactly 
        one measurement per combination, consider dropping higher-order 
        interactions or collecting more data.
        """)

if __name__=="__main__":
    loc=sys.argv[1]
    print(loc)
    loc = f"{loc}"
    anova(loc)
