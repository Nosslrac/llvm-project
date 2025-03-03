#!/usr/bin/env python3
import re
import sys
from collections import defaultdict
import numpy as np

def parse_out_file(filename):
    tasks = {}
    with open(filename, "r") as f:
        lines = f.readlines()

    # Header regex that captures Task ID, Routine ID, CPU ID, and Thread ID.
    header_pattern = r'Counters for Task (0x[0-9a-fA-F]+) executing Routine (0x[0-9a-fA-F]+) on CPU#(\d+)\s+\(T#(\d+)\):'
    param_pattern = r'-\s*(.*?)\s*=\s*([0-9]*\.?[0-9]+)'

    i = 0
    while i < len(lines):
        line = lines[i].strip()
        header_match = re.search(header_pattern, line)
        if header_match:
            task_id = header_match.group(1)
            routine_id = header_match.group(2)
            cpu_id = int(header_match.group(3))
            thread_id = int(header_match.group(4))
            params = {}
            i += 1  # Move to parameter lines

            while i < len(lines) and lines[i].lstrip().startswith("-"):
                param_line = lines[i].strip()
                param_match = re.match(param_pattern, param_line)
                if param_match:
                    key = param_match.group(1)
                    value_str = param_match.group(2)
                    value = float(value_str) if '.' in value_str else int(value_str)
                    params[key] = value
                i += 1

            tasks[task_id] = {
                "routine": routine_id,
                "parameters": params,
                "cpu": cpu_id,
                "thread": thread_id
            }
        else:
            i += 1

    return tasks

def group_tasks_by_routine(tasks):
    routines = defaultdict(list)
    for task_id, info in tasks.items():
        routines[info["routine"]].append((task_id, info))
    return routines

def calculate_pearson(x, y):
    if len(x) < 2:
        return None
    corr_matrix = np.corrcoef(x, y)
    return corr_matrix[0, 1]

def write_results_to_file(routines, output_filename):
    with open(output_filename, "w") as outf:
        for routine_id, tasks_list in routines.items():
            outf.write(f"Routine {routine_id}:\n")
            # Sort tasks by ascending CPU ID.
            tasks_sorted = sorted(tasks_list, key=lambda item: item[1]["cpu"])
            for task_id, info in tasks_sorted:
                outf.write(f"  Task {task_id} (CPU#{info['cpu']}, T#{info['thread']}):\n")
                for key, value in info["parameters"].items():
                    outf.write(f"    {key}: {value}\n")
                outf.write("\n")
            
            # Collect all parameter keys (excluding 'Execution time')
            param_keys = set()
            for _, info in tasks_list:
                param_keys.update(info["parameters"].keys())
            if "Execution time" in param_keys:
                param_keys.remove("Execution time")
            
            outf.write("  Pearson correlation with Execution time:\n")
            for param in sorted(param_keys):
                exec_times = []
                param_values = []
                for _, info in tasks_list:
                    params = info["parameters"]
                    if "Execution time" in params and param in params:
                        exec_times.append(params["Execution time"])
                        param_values.append(params[param])
                if len(exec_times) < 2:
                    corr = "N/A"
                else:
                    corr_val = calculate_pearson(exec_times, param_values)
                    corr = f"{corr_val:.6f}"
                outf.write(f"    {param}: {corr}\n")
            outf.write("\n")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python script.py <input_filename>")
        sys.exit(1)
        
    input_filename = sys.argv[1]
    tasks = parse_out_file(input_filename)
    routines = group_tasks_by_routine(tasks)
    output_filename = "corr_out.txt"
    write_results_to_file(routines, output_filename)
    print(f"Results written to {output_filename}")
