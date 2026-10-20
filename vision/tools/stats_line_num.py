# -*- coding:utf-8 -*-

import os


def count_file_lines(file_name):
    with open(file_name, "rb") as file:
        return sum(1 for _ in file)


def collect_stats(root_dir, skip_rules):
    count = 0
    num_file = 0
    for root, dirs, files in os.walk(root_dir):
        for file in files:
            fn = os.path.join(root, file)
            if any(rule in fn for rule in skip_rules):
                continue
            try:
                count += count_file_lines(fn)
                num_file += 1
            except OSError as error:
                print(f"skip unreadable file: {fn} ({error})")
    return count, num_file


renderer_skip_rules = [
    "ext\\",
    "tests",
    "srgb2spec.h",
    "cpplibs\\",
    "__pycache__\\",
    ".json",
    "metal_ior.inl.h",
    "ltc_sheen_table.h",
    "precomputed_table.h",
    "stats_line_num.py",
    "test.py",
    "_embed.h",
    "jitify",
    ".natvis",
    "json.hpp",
    "ocarina",
]

framework_skip_rules = [
    "ext\\",
    "tests",
    "vulkan",
    "stats_line_num.py",
    "sdk_pt",
    ".natvis",
]

python_skip_rules = [
    "toml",
    "cpplibs\\",
    "__pycache__\\",
]


r_count, r_num_file = collect_stats(os.path.join(os.getcwd(), "src"), renderer_skip_rules)
print("renderer stats ", r_count, r_num_file)

count, num_file = collect_stats(os.path.join(os.getcwd(), "src/ocarina/src"), framework_skip_rules)
print("frame work stats ", count, num_file)

py_count, py_file_num = collect_stats(os.path.join(os.getcwd(), "python"), python_skip_rules)
print("python stats ", py_count, py_file_num)

print("total stats: ", count + r_count + py_count, num_file + r_num_file + py_file_num)