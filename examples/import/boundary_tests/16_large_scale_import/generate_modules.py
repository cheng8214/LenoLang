#!/usr/bin/env python3
# 生成大规模导入测试模块

import os

def generate_module(index):
    """生成单个模块文件"""
    filename = f"module_{index:02d}.leno"
    content = f'''// 大规模导入测试 - 模块 {index:02d}
export var id = {index}
export var name = "module_{index:02d}"
export func get_value() {{ return id * 10 }}
export func compute(int x) {{ return x + id }}
'''
    return filename, content

def main():
    # 创建 20 个模块
    num_modules = 20
    directory = os.path.dirname(os.path.abspath(__file__))

    for i in range(1, num_modules + 1):
        filename, content = generate_module(i)
        filepath = os.path.join(directory, filename)
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(content)
        print(f"Generated: {filename}")

    print(f"\nTotal {num_modules} modules generated.")

if __name__ == "__main__":
    main()
