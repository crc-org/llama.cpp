#!/usr/bin/env python3
"""
Script to extract ggml_metal_device_supports_op function from ggml-metal-device.m
and update it in ggml-metal-remoting.cpp.

The function prototype and first 3 lines are hardcoded as specified, and the rest
of the function body is copied from the source file.
"""

import re
import sys
import os

# File paths
SOURCE_FILE = "ggml/src/ggml-metal/ggml-metal-device.m"
TARGET_FILE = "ggml/src/ggml-remotingfrontend/ggml-metal-remoting.cpp"

# Hardcoded prototype and first 3 lines for the target function
TARGET_PROTOTYPE = "bool ggml_metal_device_supports_op(const struct ggml_backend_metal_device_context *dev_ctx, const struct ggml_tensor * op) {"
TARGET_FIRST_LINES = """    const bool has_simdgroup_mm        = dev_ctx->has_simdgroup_mm;
    const bool has_simdgroup_reduction = dev_ctx->has_simdgroup_reduction;
    const bool has_bfloat              = dev_ctx->has_bfloat;"""

def extract_function_body(source_file):
    """Extract the function body from the source file, starting after the 3rd line."""
    try:
        with open(source_file, 'r') as f:
            content = f.read()

        # Find the function start
        pattern = r'bool ggml_metal_device_supports_op\([^)]+\)\s*\{'
        match = re.search(pattern, content)

        if not match:
            print(f"Error: Could not find function declaration in {source_file}")
            return None

        # Find the start of the function body
        func_start = match.end()

        # Extract lines to find where to start copying from (after the 3rd variable declaration)
        lines = content[func_start:].split('\n')

        # Skip the first few lines (variable declarations) and find the start of the actual logic
        skip_lines = 0
        for i, line in enumerate(lines):
            if 'const bool has_bfloat' in line:
                # Find the end of this declaration (next non-blank line that's not a variable declaration)
                for j in range(i + 1, len(lines)):
                    stripped = lines[j].strip()
                    if stripped and not stripped.startswith('const bool'):
                        skip_lines = j
                        break
                break

        if skip_lines == 0:
            print("Error: Could not find the end of variable declarations")
            return None

        # Get the remaining function body
        remaining_lines = lines[skip_lines:]

        # Find the matching closing brace for the function
        brace_count = 0
        end_line = 0

        for i, line in enumerate(remaining_lines):
            brace_count += line.count('{') - line.count('}')
            if brace_count < 0:  # Found the closing brace
                end_line = i
                break

        if end_line == 0:
            print("Error: Could not find the end of function")
            return None

        # Extract the function body (excluding the final closing brace)
        function_body = '\n'.join(remaining_lines[:end_line])

        return function_body.rstrip()

    except FileNotFoundError:
        print(f"Error: Source file {source_file} not found")
        return None
    except Exception as e:
        print(f"Error reading source file: {e}")
        return None

def update_target_file(target_file, new_function_body):
    """Update the target file with the new function implementation."""
    try:
        with open(target_file, 'r') as f:
            content = f.read()

        # Find the existing function
        pattern = r'(bool ggml_metal_device_supports_op\([^)]+\)\s*\{).*?(\n\})'
        match = re.search(pattern, content, re.DOTALL)

        if not match:
            print(f"Error: Could not find function in target file {target_file}")
            return False

        # Construct the new function
        new_function = f"{TARGET_PROTOTYPE}\n{TARGET_FIRST_LINES}\n\n{new_function_body}\n}}"

        # Replace the old function with the new one
        new_content = content[:match.start()] + new_function + content[match.end():]

        # Write back to file
        with open(target_file, 'w') as f:
            f.write(new_content)

        print(f"Successfully updated {target_file}")
        return True

    except FileNotFoundError:
        print(f"Error: Target file {target_file} not found")
        return False
    except Exception as e:
        print(f"Error updating target file: {e}")
        return False

def main():
    """Main function to orchestrate the update process."""
    print("Extracting ggml_metal_device_supports_op from source file...")

    # Check if files exist
    if not os.path.exists(SOURCE_FILE):
        print(f"Error: Source file {SOURCE_FILE} not found")
        sys.exit(1)

    if not os.path.exists(TARGET_FILE):
        print(f"Error: Target file {TARGET_FILE} not found")
        sys.exit(1)

    # Extract function body
    function_body = extract_function_body(SOURCE_FILE)
    if function_body is None:
        sys.exit(1)

    print(f"Extracted {len(function_body.split())} lines of function body")

    # Update target file
    if update_target_file(TARGET_FILE, function_body):
        print("Update completed successfully!")
    else:
        sys.exit(1)

if __name__ == "__main__":
    main()