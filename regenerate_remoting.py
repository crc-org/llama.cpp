#!/usr/bin/env python3
"""
Script to completely regenerate the GGML remoting codebase from YAML configuration.

This script reads api_functions.yaml and regenerates all the header files and
implementation templates for the GGML remoting layer.

Usage:
  python regenerate_remoting.py

The script will:
1. Read api_functions.yaml configuration
2. Generate updated header files
3. Generate implementation templates in dedicated files
4. Show a summary of what was generated
"""

import yaml
from typing import Dict, List, Any, Tuple
from pathlib import Path
import os

class RemotingCodebaseGenerator:
    def __init__(self, yaml_path: str = "ggmlremoting_functions.yaml"):
        """Initialize the generator with the YAML configuration."""
        self.yaml_path = yaml_path

        if not Path(yaml_path).exists():
            raise FileNotFoundError(f"Configuration file {yaml_path} not found")

        with open(yaml_path, 'r') as f:
            self.config = yaml.safe_load(f)

        self.function_groups = self.config['function_groups']
        self.function_metadata = self.config['function_metadata']
        self.naming_patterns = self.config['naming_patterns']
        self.config_data = self.config['config']

    def generate_enum_name(self, group_name: str, function_name: str) -> str:
        """Generate the APIR_COMMAND_TYPE enum name for a function."""
        prefix = self.naming_patterns['enum_prefix']
        return f"{prefix}{group_name.upper()}_{function_name.upper()}"

    def generate_backend_function_name(self, group_name: str, function_name: str) -> str:
        """Generate the backend function name."""
        function_key = f"{group_name}_{function_name}"
        overrides = self.naming_patterns.get('backend_function_overrides', {})

        if function_key in overrides:
            return overrides[function_key]

        prefix = self.naming_patterns['backend_function_prefix']
        return f"{prefix}{group_name}_{function_name}"

    def generate_frontend_function_name(self, group_name: str, function_name: str) -> str:
        """Generate the frontend function name."""
        prefix = self.naming_patterns['frontend_function_prefix']
        return f"{prefix}{group_name}_{function_name}"

    def get_enabled_functions(self) -> List[Dict[str, Any]]:
        """Get all enabled functions with their metadata."""
        functions = []
        enum_value = 0

        for group_name, group_data in self.function_groups.items():
            for func in group_data['functions']:
                if func.get('enabled', True):
                    function_key = f"{group_name}_{func['name']}"
                    metadata = self.function_metadata.get(function_key, {})

                    functions.append({
                        'group_name': group_name,
                        'function_name': func['name'],
                        'enum_name': self.generate_enum_name(group_name, func['name']),
                        'enum_value': enum_value,
                        'backend_function': self.generate_backend_function_name(group_name, func['name']),
                        'frontend_function': self.generate_frontend_function_name(group_name, func['name']),
                        'frontend_return': metadata.get('frontend_return', 'void'),
                        'frontend_extra_params': metadata.get('frontend_extra_params', []),
                        'group_description': group_data['group_description'],
                        'newly_added': func.get('newly_added', False)
                    })
                    enum_value += 1

        return functions

    def generate_apir_backend_header(self) -> str:
        """Generate the complete apir_backend.h file."""
        functions = self.get_enabled_functions()

        # Generate the enum section
        enum_lines = ["typedef enum ApirBackendCommandType {"]
        current_group = None

        for func in functions:
            # Add comment for new group
            if func['group_name'] != current_group:
                enum_lines.append("")
                enum_lines.append(f"  /* {func['group_description']} */")
                current_group = func['group_name']

            enum_lines.append(f"  {func['enum_name']} = {func['enum_value']},")

        # Add the count
        total_count = len(functions)
        enum_lines.append(f"\n  // last command_type index + 1")
        enum_lines.append(f"  APIR_BACKEND_DISPATCH_TABLE_COUNT = {total_count},")
        enum_lines.append("} ApirBackendCommandType;")

        # Full header template
        header_content = chr(10).join(enum_lines) + "\n"

        return header_content

    def generate_backend_dispatched_header(self) -> str:
        """Generate the complete backend-dispatched.h file."""
        functions = self.get_enabled_functions()

        # Function declarations
        decl_lines = []
        current_group = None

        for func in functions:
            if func['group_name'] != current_group:
                decl_lines.append(f"\n/* {func['group_description']} */")
                current_group = func['group_name']

            signature = "uint32_t"
            params = "struct vn_cs_encoder *enc, struct vn_cs_decoder *dec, struct virgl_apir_context *ctx"
            decl_lines.append(f"{signature} {func['backend_function']}({params});")

        # Switch cases
        switch_lines = []
        current_group = None

        for func in functions:
            if func['group_name'] != current_group:
                switch_lines.append(f"  /* {func['group_description']} */")
                current_group = func['group_name']

            switch_lines.append(f"  case {func['enum_name']}: return \"{func['backend_function']}\";")

        # Dispatch table
        table_lines = []
        current_group = None

        for func in functions:
            if func['group_name'] != current_group:
                table_lines.append(f"\n  /* {func['group_description']} */")
                table_lines.append("")
                current_group = func['group_name']


            table_lines.append(f"  /* {func['enum_name']}  = */ {func['backend_function']},")
        total_count = len(functions)

        header_content = f'''\
#pragma once

{chr(10).join(decl_lines)}

static inline const char *backend_dispatch_command_name(ApirBackendCommandType type)
{{
  switch (type) {{
{chr(10).join(switch_lines)}

  default: return "unknown";
  }}
}}

extern "C" {{
static const backend_dispatch_t apir_backend_dispatch_table[APIR_BACKEND_DISPATCH_TABLE_COUNT] = {{
  {chr(10).join(table_lines)}
}};
}}
'''
        return header_content

    def generate_virtgpu_forward_header(self) -> str:
        """Generate the complete virtgpu-forward.h file."""
        functions = self.get_enabled_functions()

        decl_lines = []
        current_group = None

        for func in functions:
            if func['group_name'] != current_group:
                decl_lines.append("")
                decl_lines.append(f"/* {func['group_description']} */")
                current_group = func['group_name']

            # Build parameter list
            params = [self.naming_patterns['frontend_base_param']]
            params.extend(func['frontend_extra_params'])
            param_str = ', '.join(params)

            decl_lines.append(f"{func['frontend_return']} {func['frontend_function']}({param_str});")

        header_content = f'''\
#pragma once
{chr(10).join(decl_lines)}
'''
        return header_content

    def regenerate_codebase(self) -> None:
        """Regenerate the entire remoting codebase."""
        print("🔄 Regenerating GGML Remoting Codebase...")
        print("=" * 50)

        # Use base_path from config
        base_path = self.config_data.get('base_path', 'ggml/src')
        files_config = self.config_data.get('files', {})

        # Build file paths using config
        apir_backend_path = os.path.join(base_path, files_config.get('apir_backend_header', 'ggml-remotingbackend/shared/apir_backend.gen.h'))
        backend_dispatched_path = os.path.join(base_path, files_config.get('backend_dispatched_header', 'ggml-remotingbackend/backend-dispatched.gen.h'))
        virtgpu_forward_path = os.path.join(base_path, files_config.get('virtgpu_forward_header', 'ggml-remotingfrontend/virtgpu-forward.gen.h'))

        # Create output directories for each file
        os.makedirs(os.path.dirname(apir_backend_path), exist_ok=True)
        os.makedirs(os.path.dirname(backend_dispatched_path), exist_ok=True)
        os.makedirs(os.path.dirname(virtgpu_forward_path), exist_ok=True)

        # Generate header files
        print("📁 Generating header files...")

        apir_backend_content = self.generate_apir_backend_header()
        with open(apir_backend_path, "w") as f:
            f.write(apir_backend_content)
        print(f"   ✅ {apir_backend_path}")

        backend_dispatched_content = self.generate_backend_dispatched_header()
        with open(backend_dispatched_path, "w") as f:
            f.write(backend_dispatched_content)
        print(f"   ✅ {backend_dispatched_path}")

        virtgpu_forward_content = self.generate_virtgpu_forward_header()
        with open(virtgpu_forward_path, "w") as f:
            f.write(virtgpu_forward_content)
        print(f"   ✅ {virtgpu_forward_path}")

        # Generate summary
        functions = self.get_enabled_functions()
        total_functions = len(functions)

        print("\n📊 Generation Summary:")
        print("=" * 50)
        print(f"   Total functions: {total_functions}")
        print(f"   Function groups: {len(self.function_groups)}")
        print(f"   Header files: 3")

def main():
    try:
        generator = RemotingCodebaseGenerator()
        generator.regenerate_codebase()
    except Exception as e:
        print(f"❌ Error: {e}")
        exit(1)

if __name__ == "__main__":
    main()
