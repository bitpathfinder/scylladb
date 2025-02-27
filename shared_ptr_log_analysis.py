import re
import argparse
from collections import defaultdict

def parse_log_file(log_file_path):
    # Regular expressions to match different operations
    constructor_pattern = re.compile(r'shared_ptr\(shared_ptr_count_for<T>\* b\) (?P<ptr>0x[0-9a-f]+) for (?P<resource>0x[0-9a-f]+)')
    move_constructor_pattern = re.compile(r'shared_ptr\(shared_ptr&& x\) (?P<ptr>0x[0-9a-f]+) for (?P<resource>0x[0-9a-f]+), from (?P<from_ptr>0x[0-9a-f]+)')
    copy_constructor_pattern = re.compile(r'shared_ptr\(const shared_ptr& x\) (?P<ptr>0x[0-9a-f]+) for (?P<resource>0x[0-9a-f]+), from (?P<from_ptr>0x[0-9a-f]+)')
    assignment_pattern = re.compile(r'shared_ptr& operator=\(const shared_ptr& x\) (?P<ptr>0x[0-9a-f]+) for (?P<resource>0x[0-9a-f]+), from (?P<from_ptr>0x[0-9a-f]+)')
    move_assignment_pattern = re.compile(r'shared_ptr& operator=\(shared_ptr&& x\) (?P<ptr>0x[0-9a-f]+) for (?P<resource>0x[0-9a-f]+), from (?P<from_ptr>0x[0-9a-f]+)')
    destructor_pattern = re.compile(r'~shared_ptr\(\) (?P<ptr>0x[0-9a-f]+) for (?P<resource>0x[0-9a-f]+)')

    # Dictionary to track operations
    operations = defaultdict(list)
    destructors = set()
    moved_from = set()

    # Read log file
    with open(log_file_path, 'r') as file:
        log_data = file.readlines()

    # Parse log data
    for line in log_data:
        constructor_match = constructor_pattern.search(line)
        if constructor_match:
            ptr = constructor_match.group('ptr')
            operations[ptr].append(f"Constructor: {line.strip()}")
            continue

        move_constructor_match = move_constructor_pattern.search(line)
        if move_constructor_match:
            ptr = move_constructor_match.group('ptr')
            from_ptr = move_constructor_match.group('from_ptr')
            operations[ptr].append(f"Move Constructor: {line.strip()}")
            operations[from_ptr].append(f"Moved to {ptr}: {line.strip()}")
            moved_from.add(from_ptr)
            continue

        copy_constructor_match = copy_constructor_pattern.search(line)
        if copy_constructor_match:
            ptr = copy_constructor_match.group('ptr')
            from_ptr = copy_constructor_match.group('from_ptr')
            operations[ptr].append(f"Copy Constructor: {line.strip()}")
            operations[from_ptr].append(f"Copied to {ptr}: {line.strip()}")
            continue

        assignment_match = assignment_pattern.search(line)
        if assignment_match:
            ptr = assignment_match.group('ptr')
            from_ptr = assignment_match.group('from_ptr')
            operations[ptr].append(f"Assignment: {line.strip()}")
            operations[from_ptr].append(f"Assigned to {ptr}: {line.strip()}")
            continue

        move_assignment_match = move_assignment_pattern.search(line)
        if move_assignment_match:
            ptr = move_assignment_match.group('ptr')
            from_ptr = move_assignment_match.group('from_ptr')
            operations[ptr].append(f"Move Assignment: {line.strip()}")
            operations[from_ptr].append(f"Move Assigned to {ptr}: {line.strip()}")
            moved_from.add(from_ptr)
            continue

        destructor_match = destructor_pattern.search(line)
        if destructor_match:
            ptr = destructor_match.group('ptr')
            destructors.add(ptr)

    # Filter out shared_ptr instances that have a destructor or end with being moved from
    filtered_operations = {ptr: ops for ptr, ops in operations.items() if ptr not in destructors and ptr not in moved_from}

    return filtered_operations

def main():
    parser = argparse.ArgumentParser(description="Analyze shared_ptr log file.")
    parser.add_argument("log_file", help="Path to the shared_ptr log file.")
    args = parser.parse_args()

    operations = parse_log_file(args.log_file)

    for ptr, ops in operations.items():
        print(f"Operations for shared_ptr {ptr}:")
        for op in ops:
            print(f"  {op}")
        print()

if __name__ == "__main__":
    main()
