import re
import sys
import os

def convert_for_to_taskloop(code):
    # Regular expression to match the pragma omp for and the following loop
    # omp_for_pattern = re.compile(
    #     r'#pragma\s+omp\s+for\s*'
    #     # r'(reduction\s*\(\s*(.*?)\s*\))?\s*'
    #     r'(?:(reduction\s*\(\s*([^:]+)\s*:\s*((?:[^,\)]+(?:\[[^\]]+\])?(?:,\s*)?)+)\s*\))?\s*)*'
    #     r'(private\s*\(\s*(.*?)\s*\))?\s*'
    #     r'(firstprivate\s*\(\s*(.*?)\s*\))?\s*'
    #     r'(lastprivate\s*\(\s*(.*?)\s*\))?\s*'
    #     r'(nowait\s*)?'
    #     r'(schedule\s*\(\s*dynamic\s*\)\s*)?\n'
    #     r'\s*for\s*\((.*?)\)\s*{'
    # )
    # omp_for_pattern = re.compile(
    #     r'#pragma\s+omp\s+for\s*'
    #     r'(?:(reduction\s*\(\s*([^:]+)\s*:\s*((?:[^,\)]+(?:\[[^\]]+\])?(?:,\s*)?)+)\s*\))?\s*)*'
    #     r'(private\s*\(\s*(.*?)\s*\))?\s*'
    #     r'(firstprivate\s*\(\s*(.*?)\s*\))?\s*'
    #     r'(lastprivate\s*\(\s*(.*?)\s*\))?\s*'
    #     r'(nowait\s*)?'
    #     r'(schedule\s*\(\s*(?:dynamic|guided|auto|runtime)(?:\s*,\s*\d+)?\s*\)\s*)?\n'
    #     r'\s*for\s*\((.*?)\)\s*{'
    # )
    omp_for_pattern = re.compile(
        r'#pragma\s+omp\s+for\s*'
        r'(?:'
            r'(reduction\s*\(\s*([+\-\*/&|^%]+|&&|\|\||min|max)\s*:\s*((?:[^,\)]+(?:\[[^\]]+\])?(?:,\s*)?)+)\s*\)\s*)|'
            r'(private\s*\(\s*(.*?)\s*\)\s*)|'
            r'(firstprivate\s*\(\s*(.*?)\s*\)\s*)|'
            r'(lastprivate\s*\(\s*(.*?)\s*\)\s*)|'
            r'(nowait\s*)|'
            r'(schedule\s*\(\s*(?:dynamic|guided|auto|runtime)(?:\s*,\s*\d+)?\s*\)\s*)'
        r')*'
        r'for\s*\((.*?)\)\s*{'
    )

    def parse_omp_pragma(match):
        clauses = {
            'reduction': None,
            'private': None,
            'firstprivate': None,
            'lastprivate': None,
            'nowait': None,
            'schedule': None
        }
        
        full_match = match.group(0)
        
        # Find all occurrences of each clause
        reduction_matches = re.findall(r'reduction\s*\((.*?)\)', full_match)
        private_matches = re.findall(r'private\s*\((.*?)\)', full_match)
        firstprivate_matches = re.findall(r'firstprivate\s*\((.*?)\)', full_match)
        lastprivate_matches = re.findall(r'lastprivate\s*\((.*?)\)', full_match)
        
        if reduction_matches:
            clauses['reduction'] = reduction_matches[0]
        if private_matches:
            clauses['private'] = private_matches[0]
        if firstprivate_matches:
            clauses['firstprivate'] = firstprivate_matches[0]
        if lastprivate_matches:
            clauses['lastprivate'] = lastprivate_matches[0]
        
        clauses['nowait'] = 'nowait' in full_match
        
        schedule_match = re.search(r'schedule\s*\((.*?)\)', full_match)
        if schedule_match:
            clauses['schedule'] = schedule_match.group(1)
        
        # The loop control is still the last group in the main regex
        clauses['loop_control'] = match.group(12)
        
        return clauses

    def extract_full_loop_body(code, start_pos):
        """
        Extracts the full body of a loop starting from `start_pos`, including nested loops and code.

        Args:
            code (str): The entire source code as a string.
            start_pos (int): The starting position of the loop body.

        Returns:
            str: The complete loop body including all nested content.
        """
        # Remove block comments
        # code = re.sub(r'/\*.*?\*/', '', code, flags=re.DOTALL)

        stack = []
        loop_body = []
        current_pos = start_pos
        found_closing_brace = False
        stack_used = False

        while current_pos < len(code):
            char = code[current_pos]
            if char == '{':
                stack_used = True
                stack.append('{')
                loop_body.append(char)  # Add '{' to the loop body
                # print(f"brace_stack: {stack}")  # Debugging line    
                # print(f"current_pos: {current_pos}, char: {char}")  # Debugging line
            elif char == '}':
                if stack:
                    stack.pop()
                    # print(f"brace_stack: {stack}")  # Debugging line
                if not stack:
                    if stack_used:
                        # Check if this is the correct closing brace for the `k` loop
                        next_char_pos = current_pos + 1
                        # Skip any whitespace and newlines to find the next meaningful character
                        while next_char_pos < len(code) and code[next_char_pos].isspace():
                            next_char_pos += 1
                        
                        # If the next character is another '}', it means this is the correct closing brace
                        if next_char_pos < len(code):
                            if code[next_char_pos] == '}':
                                loop_body.append(char)  # Add the correct closing brace
                                found_closing_brace = True
                                break
                            else:
                                # This is not the correct closing brace, continue adding to the loop body
                                loop_body.append(char)
                                # print(f"2 current_pos: {current_pos}, char: {char}")  # Debugging line
                    else:
                        found_closing_brace = True
                        break
                else:
                    # This is a nested closing brace, add it to the loop body
                    loop_body.append(char)
            else:
                if not stack and stack_used:
                        next_char_pos = current_pos + 1
                        # Skip any whitespace and newlines to find the next meaningful character
                        while next_char_pos < len(code) and code[next_char_pos].isspace():
                            next_char_pos += 1
                        if next_char_pos < len(code):
                            if code[next_char_pos] == '}':
                                loop_body.append(char)  # Add the correct closing brace
                                found_closing_brace = True
                                break
                            else:
                                # This is not the correct closing brace, continue adding to the loop body
                                loop_body.append(char)
                else:
                    loop_body.append(char)
                    # print(f"1 current_pos: {current_pos}, char: {char}")  # Debugging line
            current_pos += 1

        if not found_closing_brace:
            print("Warning: Loop body might not be fully captured.")

        return ''.join(loop_body).rstrip()

    def remove_comments(loop_body):
        # Regex to match comments in the format /* ... */
        comment_pattern = re.compile(r'/\*.*?\*/', re.DOTALL)
        # Replace comments with an empty string
        loop_body = re.sub(comment_pattern, '', loop_body)
        return loop_body

    def extract_other_loop_variables(loop_body):
        loop_vars = []
        
        # Regular expression to match 'for' loop statements
        loop_pattern = re.compile(r'for\s*\(([^)]+)\)')
        matches = loop_pattern.findall(loop_body)
        
        # Debug: Print all detected for loops
        # print(f"Raw for loop matches: {matches}")  # Debugging line

        # Iterate over all detected for loops and extract variables
        for loop_control_line in matches:
            extracted_vars = extract_loop_variables(loop_control_line)
            for var in extracted_vars:
                if var not in loop_vars:  # Ensure no duplicates, but maintain order
                    loop_vars.append(var)
        
        # print(f"Detected loop variables: {loop_vars}")  # Debugging line
        return loop_vars
        
    def extract_loop_variables(loop_control_line):
        loop_vars = []
        init_exprs = loop_control_line.split(';')
        # Debug: Print initial expressions
        # print(f"Initial expressions: {init_exprs}")
        # Regular expression to match variable assignments (e.g., "i = 0")
        var_pattern = re.compile(r'(\s+)?(\w+)\s*=')
        
        for expr in init_exprs:
            expr = expr.strip()
            var_match = var_pattern.match(expr)
            if var_match:
                loop_vars.append(var_match.group(2))
        
        # Debug: Print detected loop variables for this specific loop
        # print(f"Detected variables in loop control: {loop_vars}")  # Debugging line
        return loop_vars 

    def extract_other_loop_variables_for_private(loop_body):
        loop_vars = []
        # Regular expression to match 'for' loop statements
        loop_pattern = re.compile(r'for\s*\(([^)]+)\)')
        matches = loop_pattern.findall(loop_body)
        
        # Debug: Print all detected for loops
        # print(f"Raw for loop matches: {matches}")  # Debugging line

        # Iterate over all detected for loops and extract variables
        for loop_control_line in matches:
            loop_vars.extend(extract_loop_variables(loop_control_line))
        
        # Remove duplicates and print final list of loop variables
        loop_vars = list(set(loop_vars))
        # print(f"Detected loop variables for private: {loop_vars}")  # Debugging line
        return loop_vars

    # Function to extract loop variables from the loop control statement
    def extract_loop_variables_for_private(loop_control_line):
        loop_vars = []
        init_exprs = loop_control_line.split(';')
        # Debug: Print initial expressions
        # print(f"Initial expressions: {init_exprs}")
        
        # Regular expression to match variable assignments, with or without "int" (e.g., "i = 0" or "int i = 0")
        var_pattern = re.compile(r'(\s*int\s+)?(\w+)\s*=')
        
        for expr in init_exprs:
            expr = expr.strip()
            var_match = var_pattern.match(expr)
            # Only append the variable if "int" is not present
            if var_match and not var_match.group(1):
                loop_vars.append(var_match.group(2))
        
        # Debug: Print detected loop variables for this specific loop
        # print(f"Detected variables in loop control for private: {loop_vars}")  # Debugging line
        return loop_vars
    
    def find_previous_non_space(loop_body, current_pos):
        for i in range(current_pos - 1, -1, -1):
            if not loop_body[i].isspace():
                return loop_body[i]
        return None

    def count_nested_loops(loop_body):
        brace_stack = []
        max_depth = 0
        current_depth = 0
        code_after_last_brace = False
        start_pop = False
        in_for_loop = [False] * 100 # Assuming a maximum of 100 nested levels
        out_for_loop_codes = False
        brace_pos = 0

        # Remove comments and string literals to avoid false positives
        loop_body = re.sub(r'//.*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"', '', loop_body)

        # Regex to detect 'for' loop
        for_pattern = re.compile(r'\bfor\b\s*\(.*?\)\s*{')
        
        pos = 0
        while pos < len(loop_body):
            if loop_body[pos] == '{':
                if in_for_loop:
                    brace_stack.append('{')
                    current_depth += 1
                    max_depth = max(max_depth, current_depth)
                    brace_pos = pos
                    # Debugging lines
                    print(f"[JING]Entered brace, current_depth: {current_depth}, max_depth: {max_depth}, race_stack: {brace_stack}")
                    if start_pop:
                        print(f"[JING]there is new brace after closed braces, quit now")
                        return max_depth - 1
                    # in_for_loop = False  # Reset after processing a 'for' loop block
            elif loop_body[pos] == '}':
                if brace_stack:
                    brace_stack.pop()
                    current_depth -= 1
                    start_pop = True
                    if not brace_stack:
                        in_for_loop = False
                    print(f"[JING]Exited brace, current_depth: {current_depth}, brace_stack: {brace_stack}")
                else:
                    return 0  # Mismatched closing brace
            elif loop_body[pos:pos + 3] == 'for':
                print(f"[JING]loop_body[pos:pos + 3]: {loop_body[pos:pos + 3]}")
                # Check if this is indeed a 'for' loop and not just a variable name or other occurrence of 'for'
                if for_pattern.match(loop_body, pos):
                    print(f"[JING]'for' loop detected at position {pos}")
                    if not in_for_loop:
                        in_for_loop = True  # We're inside a 'for' loop now
                    prev_char = find_previous_non_space(loop_body, pos)
                    if prev_char != None:
                        print(f"[JING]prev_char: {prev_char}")
                        if prev_char != '{':
                            print(f"[JING] prrvious char is not left brace, quit now")
                            return current_depth

            elif not loop_body[pos].isspace():
                print(f"[JING]Non-whitespace character detected: {loop_body[pos]}") # Detect non-whitespace characters outside of loop blocks
                if not in_for_loop: # not in a for loop
                    out_for_loop_codes = True
                    print(f"[JING] Detected code outside of for loop, out_for_loop_codes: {out_for_loop_codes}")
                    return 0
                elif not brace_stack and max_depth > 0:
                    code_after_last_brace = True
                    print(f"[JING]Code after brace detected, code_after_last_brace: {code_after_last_brace}")
                    return 0
                elif in_for_loop and brace_stack and start_pop:
                    if find_previous_non_space(loop_body, pos) == '}':
                        code_after_last_brace = True
                        print(f"[JING]Code after brace detected, code_after_last_brace: {code_after_last_brace}")
                        return current_depth

            pos += 1

        if not code_after_last_brace or not out_for_loop_codes:
            return max_depth

        return 0  # Return 0 if the loop structure is somehow invalid or incomplete

    # Function to detect variables that are assigned in the loop body
    def detect_assigned_variables(loop_body, loop_vars):
        assigned_vars = set()
        all_vars = set()
        declared_vars = set()

        # Step 1: Remove comments from the loop body
        loop_body = remove_comments(loop_body)

        array_access_pattern = re.compile(r'(\w+)\s*(\[.*?\])?\s*=')
        
        # declaration_pattern = re.compile(r'\b(?:int|float|double|char|long|short|unsigned|signed|bool)\b\s+([\w]+(?:\s*\[.*?\])*)(?:\s*,\s*([\w]+(?:\s*\[.*?\])*))*')
        # Primary pattern to match the type and the first variable
        declaration_pattern = re.compile(
            r'\b(?:int|float|double|char|long|short|unsigned|signed|bool|dcomplex)\b\s+([\w]+(?:\s*\[.*?\])*)'
        )

        # Secondary pattern to match subsequent variables
        subsequent_vars_pattern = re.compile(r',\s*([\w]+(?:\s*\[.*?\])*)')

        # Updated reduction pattern
        # reduction_pattern = re.compile(r'(\w+)(\s*\[.*?\])?\s*([+\-*/&|^]=|=)\s*(?:\1(\s*\[.*?\])?\s*([+\-*/&|^])|.*?\1(\s*\[.*?\])?\s*([+\-*/&|^]))')
        reduction_pattern = re.compile(r'(\w+)(\[.*?\])*\s*([+\-*/&|^]?=)\s*(\1(\[.*?\])*\s*[+\-*/&|^]|\1(\[.*?\])*$)')

        # First, detect all variable declarations within the loop body
        for line in loop_body.splitlines():
            # Find the first variable declaration in the line
            first_match = declaration_pattern.search(line)
            if first_match:
                first_var = re.sub(r'\s*\[.*?\]', '', first_match.group(1))
                declared_vars.add(first_var)
                # print(f"Detected variable declaration: {first_var}")

                # Now, find all subsequent variables in the same line
                rest_of_line = line[first_match.end():]
                subsequent_matches = subsequent_vars_pattern.findall(rest_of_line)
                for var in subsequent_matches:
                    cleaned_var = re.sub(r'\s*\[.*?\]', '', var)
                    declared_vars.add(cleaned_var)
                    # print(f"Detected additional variable declaration: {cleaned_var}")

        # Now, detect assigned variables that are not declared inside the loop
        for match in array_access_pattern.finditer(loop_body):
            var_name = match.group(1)
            indices = match.group(2)
            
            # Skip the variable if it's declared within the loop body
            if var_name in declared_vars:
                continue
            
            if indices:
                # Check if indices depend on loop variables
                index_vars = re.findall(r'\b\w+\b', indices)
                # Check if all indices depend on loop variables
                if any(index_var in loop_vars for index_var in index_vars):
                    # If any index depends on loop variables, do not make it private
                    continue
                else:
                    # reduction_pattern = re.compile(r'(\w+)(\s*\[.*?\])?\s*([+\-*/&|^]=|=)\s*\1(\s*\[.*?\])?\s*([+\-*/&|^])\s*\S+')
                    # Check if this is a reduction operation
                    # line = loop_body[match.start():].split('\n')[0]  # Get the full line of the match
                    line = get_complete_statement(loop_body, match.start())
                    # reduction_match = reduction_pattern.search(line)
                    # if reduction_match:
                    #     reduction_var = reduction_match.group(1)
                    #     if reduction_var == var_name:
                    #         print(f"Detected reduction operation: {line.strip()}")  # Debugging line
                    #         continue
                    if reduction_pattern.search(line):
                        # print(f"Detected reduction operation: {line.strip()}")  # Debugging line
                        continue
                    else:
                        # Otherwise, it needs to be private
                        assigned_vars.add(var_name)
                        # print(f"Detected array assignment: {var_name} with indices: {indices}")  # Debugging line
            else:
                # Variables assigned without indexing or with partial indexing need to be private
                assigned_vars.add(var_name)
                # print(f"Detected scalar assignment: {var_name}")
            
            all_vars.add(var_name)
        
        return assigned_vars, all_vars
    
    # Function to get complete statement
    def get_complete_statement(loop_body, start_index):
        lines = loop_body[start_index:].split('\n')
        statement = []
        unclosed_parentheses = 0
        
        for line in lines:
            statement.append(line.strip())
            unclosed_parentheses += line.count('(') - line.count(')')
            unclosed_parentheses += line.count('[') - line.count(']')
            
            if line.strip().endswith(';') and unclosed_parentheses == 0:
                break
                
        return ' '.join(statement)

    # Function to detect variables that are assigned in the loop body
    def detect_assigned_variables_with_collapse(loop_body, loop_vars, collapse_level):
        assigned_vars = set()
        all_vars = set()
        declared_vars = set()

        # Step 1: Remove comments from the loop body
        loop_body = remove_comments(loop_body)

        # Extract only the relevant loop variables based on collapse_level
        relevant_loop_vars = loop_vars[:collapse_level]
        # print(f"Relevant loop variables: {relevant_loop_vars}")  # Debugging line
        array_access_pattern = re.compile(r'(\w+)\s*(\[.*?\])?\s*=')
        
        # Updated regex pattern to detect variable declarations (handles multiple declarations with any number of indices)
        declaration_pattern = re.compile(r'\b(?:int|float|double|char|long|short|unsigned|signed|bool)\b\s+([\w]+(?:\s*\[.*?\])*)(?:\s*,\s*([\w]+(?:\s*\[.*?\])*))*')

        # First, detect all variable declarations within the loop body
        for line in loop_body.splitlines():
            # Find all variable declarations in a line
            matches = declaration_pattern.findall(line)
            for match in matches:
                # Add the first variable found (remove array brackets for variable name only)
                declared_vars.add(re.sub(r'\s*\[.*?\]', '', match[0]))
                # print(f"Detected variable declaration: {match[0]}")  # Debugging line
                # Add any additional variables declared in the same statement
                for additional_var in match[1:]:
                    if additional_var:  # Ensure it's not an empty string
                        declared_vars.add(re.sub(r'\s*\[.*?\]', '', additional_var))
                        # print(f"Detected additional variable declaration: {additional_var}")  # Debugging line

        # Now, detect assigned variables that are not declared inside the loop
        for match in array_access_pattern.finditer(loop_body):
            var_name = match.group(1)
            indices = match.group(2)
            
            # Skip the variable if it's declared within the loop body
            if var_name in declared_vars:
                continue
            
            if indices:
                # Check if indices depend on loop variables
                index_vars = re.findall(r'\b\w+\b', indices)
                # Check if all indices depend on loop variables
                if any(index_var in relevant_loop_vars for index_var in index_vars):
                    # If any index depends on loop variables, do not make it private
                    continue
                else:
                    # Otherwise, it needs to be private
                    assigned_vars.add(var_name)
                    # print(f"Detected array assignment: {var_name} with indices: {indices}")  # Debugging line
            else:
                # Variables assigned without indexing or with partial indexing need to be private
                assigned_vars.add(var_name)
                # print(f"Detected scalar assignment: {var_name}")
            
            all_vars.add(var_name)
        
        return assigned_vars, all_vars
    
    def extract_reduction_variables(reduction_clause):
        """Extract the variables involved in a reduction clause."""
        reduction_vars = {}
        if not reduction_clause:
            return reduction_vars
        
        # Remove the surrounding parentheses if present
        reduction_clause = reduction_clause.strip('()')
        
        # Split the clause on the closing parenthesis to separate multiple reduction operations
        reduction_parts = reduction_clause.split(')')
        
        for part in reduction_parts:
            if ':' in part:
                op, variables = part.split(':', 1)
                op = op.strip('(').strip()
                variables = variables.strip().split(',')
                for var in variables:
                    var = var.strip()
                    if var:
                        reduction_vars[var] = op
        
        return reduction_vars

    def detect_reduction_variables_old(loop_body, loop_vars):
        reduction_vars = {}

         # Remove single-line comments
        loop_body = re.sub(r'//.*$', '', loop_body, flags=re.MULTILINE)
    
        # Remove multi-line comments
        loop_body = re.sub(r'/\*.*?\*/', '', loop_body, flags=re.DOTALL)

        print(f"Loop body:\n{loop_body}")  # Debugging line
        
        # Pattern for all reduction operations
        reduction_pattern = re.compile(r'(\w+(?:\[\w+\])*)\s*(?:(\+=|-=|\*=|&=|\|=|\^=)|(=)\s*(?:.*?([+\-*&|^])\s*\1|\1\s*([+\-*&|^]).*?))')
        
        for match in reduction_pattern.finditer(loop_body):
            var = match.group(1)
            # op = match.group(2) or match.group(4)
            op = match.group(2) or match.group(4) or match.group(5)
            print(f"Var: {var}, Op: {op}")  # Debugging line

            # Check if the variable is indexed by a loop variable
            is_loop_indexed = any(f"[{lv}]" in var for lv in loop_vars)

            if not is_loop_indexed:
                if op in ['+', '+=']:
                    reduction_vars[var] = '+'
                elif op in ['-', '-=']:
                    reduction_vars[var] = '-'
                elif op in ['*', '*=']:
                    reduction_vars[var] = '*'
                elif op in ['&', '&=']:
                    reduction_vars[var] = '&'
                elif op in ['|', '|=']:
                    reduction_vars[var] = '|'
                elif op in ['^', '^=']:
                    reduction_vars[var] = '^'
                
                print(f"Reduction variables: {reduction_vars}")  # Debugging line
        
        # Pattern for logical AND and OR
        logical_pattern = re.compile(r'(\w+)\s*=\s*\1\s*(&&|\|\|)')
        
        for match in logical_pattern.finditer(loop_body):
            var = match.group(1)
            op = match.group(2)

            # Check if the variable is indexed by a loop variable
            is_loop_indexed = any(f"[{lv}]" in var for lv in loop_vars)

            if not is_loop_indexed:
                if op == '&&':
                    reduction_vars[var] = '&&'
                elif op == '||':
                    reduction_vars[var] = '||'
            
        return reduction_vars

    def detect_reduction_variables_old2(loop_body, loop_vars):
        reduction_vars = {}

        # Remove single-line comments
        loop_body = re.sub(r'//.*$', '', loop_body, flags=re.MULTILINE)
        
        # Remove multi-line comments
        loop_body = re.sub(r'/\*.*?\*/', '', loop_body, flags=re.DOTALL)
        
        # Pattern for array reductions
        array_reduction_pattern = re.compile(r'(\w+)(\[\w+\](?:\[\w+\])*)\s*(?:(\+=|-=|\*=|&=|\|=|\^=)|(=)\s*\1(\[\w+\](?:\[\w+\])*)\s*([+\-*&|^]))')
        for match in array_reduction_pattern.finditer(loop_body):
            var = match.group(1)
            indices_str = match.group(2)
            op = match.group(3) or match.group(6)
            
            # Extract all indices
            index_pattern = re.compile(r'\[(\w+)\]')
            indices = index_pattern.findall(indices_str)
            
            print(f"Array: {var}, Indices: {indices}, Operation: {op}")
            
            if all(index not in loop_vars for index in indices):
                array_sizes = []
                for index in indices:
                    array_size = None
                    lower_bound = None
                    for line in loop_body.split('\n'):
                        bounds_match = re.search(rf'{index}\s*=\s*(\w+|\d+)\s*;\s*{index}\s*<\s*(\w+|\d+)', line)
                        # bounds_match = re.search(rf'for\s*\(\s*\w+\s+{index}\s*=\s*(\w+|\d+)\s*;\s*{index}\s*<\s*(\w+|\d+)', line)
                        if bounds_match:
                            lower_bound = bounds_match.group(1)
                            array_size = bounds_match.group(2)
                            break
                    array_sizes.append((index, lower_bound, array_size))
                    print(f"Array size: {array_size}, Lower bound: {lower_bound}")
                
                op = {'+': '+', '+=': '+', '-': '-', '-=': '-', '*': '*', '*=': '*',
                    '&': '&', '&=': '&', '|': '|', '|=': '|', '^': '^', '^=': '^'}[op]
                
                if all(size[2] for size in array_sizes):
                    reduction_key = f"{var}"
                    for index, lower_bound, size in array_sizes:
                        if lower_bound and lower_bound != '0':
                            reduction_key += f"[{lower_bound}:{size}]"
                        else:
                            reduction_key += f"[:{size}]"
                    reduction_vars[reduction_key] = op
                else:
                    reduction_key = f"{var}[{']['.join(indices)}]"
                    reduction_vars[reduction_key] = op

        # Pattern for scalar reductions
        scalar_reduction_pattern = re.compile(r'(\w+)\s*(?:(\+=|-=|\*=|&=|\|=|\^=)|(=)\s*\1\s*([+\-*&|^]))')
        
        for match in scalar_reduction_pattern.finditer(loop_body):
            var = match.group(1)
            op = match.group(2) or match.group(4)

            op = {'+': '+', '+=': '+', '-': '-', '-=': '-', '*': '*', '*=': '*',
                '&': '&', '&=': '&', '|': '|', '|=': '|', '^': '^', '^=': '^'}[op]
            
            if var not in reduction_vars:
                reduction_vars[var] = op

        return reduction_vars

    def detect_reduction_variables(loop_body, loop_vars):
        reduction_vars = {}

        # Remove single-line comments
        loop_body = re.sub(r'//.*$', '', loop_body, flags=re.MULTILINE)
        
        # Remove multi-line comments
        loop_body = re.sub(r'/\*.*?\*/', '', loop_body, flags=re.DOTALL)
        
        # Pattern for array reductions
        array_reduction_pattern = re.compile(r'(\w+)(\[\w+\](?:\[\w+\])*)\s*(?:(\+=|-=|\*=|&=|\|=|\^=)|(=)\s*\1(\[\w+\](?:\[\w+\])*)\s*([+\-*&|^]))')
        
        array_accesses = {}
        
        for match in array_reduction_pattern.finditer(loop_body):
            var = match.group(1)
            indices_str = match.group(2)
            op = match.group(3) or match.group(6)
            
            # Extract all indices
            index_pattern = re.compile(r'\[(\w+)\]')
            indices = index_pattern.findall(indices_str)
            
            # print(f"Array: {var}, Indices: {indices}, Operation: {op}")
            
            # Check if all indices are not in loop_vars
            if all(index not in loop_vars for index in indices):
                if var not in array_accesses:
                    array_accesses[var] = []
                if indices not in array_accesses[var]:
                    array_accesses[var].append(indices)
                
                op = {'+': '+', '+=': '+', '-': '-', '-=': '-', '*': '*', '*=': '*',
                    '&': '&', '&=': '&', '|': '|', '|=': '|', '^': '^', '^=': '^'}[op]
                
                reduction_vars[f"{var}[{']['.join(indices)}]"] = op

        # Find loop bounds
        loop_bounds = {}
        for line in loop_body.split('\n'):
            # Try matching both for-loop style and assignment style
            bounds_match = re.search(r'for\s*\(\s*\w+\s+(\w+)\s*=\s*(\w+|\d+)\s*;\s*\1\s*<\s*(\w+|\d+)', line)
            if not bounds_match:
                bounds_match = re.search(r'(\w+)\s*=\s*(\w+|\d+)\s*;\s*\1\s*<\s*(\w+|\d+)', line)
            
            if bounds_match:
                index, lower, upper = bounds_match.groups()
                loop_bounds[index] = (lower, upper)
                # print(f"Found bounds for {index}: lower={lower}, upper={upper}")

        # Update reduction variables with full array notation
        new_reduction_vars = {}
        for var, indices_list in array_accesses.items():
            for indices in indices_list:
                if all(index in loop_bounds for index in indices):
                    full_array_notation = f"{var}"
                    for i, index in enumerate(indices):
                        upper_bound = loop_bounds[index][1]
                        # If the upper bound is another index, use that index's upper bound
                        if upper_bound in loop_bounds:
                            upper_bound = loop_bounds[upper_bound][1]
                        full_array_notation += f"[:{upper_bound}]"
                    
                    original_key = f"{var}[{']['.join(indices)}]"
                    if original_key in reduction_vars:
                        new_reduction_vars[full_array_notation] = reduction_vars[original_key]
                    else:
                        print(f"Warning: No matching reduction variable found for {var} with indices {indices}")
                else:
                    # Keep the original notation if we can't determine bounds for all indices
                    original_key = f"{var}[{']['.join(indices)}]"
                    if original_key in reduction_vars:
                        new_reduction_vars[original_key] = reduction_vars[original_key]

        # Add scalar reductions
        scalar_reduction_pattern = re.compile(r'\b(\w+)\s*(?:(\+=|-=|\*=|&=|\|=|\^=)|(=)\s*\1\b\s*([+\-*&|^])(?!\w))')
        
        for match in scalar_reduction_pattern.finditer(loop_body):
            var = match.group(1)
            op = match.group(2) or match.group(4)

            op = {'+': '+', '+=': '+', '-': '-', '-=': '-', '*': '*', '*=': '*',
                '&': '&', '&=': '&', '|': '|', '|=': '|', '^': '^', '^=': '^'}[op]
            
            if var not in new_reduction_vars and var not in loop_vars:
                new_reduction_vars[var] = op
                # print(f"Detected scalar reduction: {var} {op}")  # Debugging line

        return new_reduction_vars
    
    def detect_reduction_variables_old(loop_body, loop_vars):
        reduction_vars = {}

        # Remove single-line comments
        loop_body = re.sub(r'//.*$', '', loop_body, flags=re.MULTILINE)
        
        # Remove multi-line comments
        loop_body = re.sub(r'/\*.*?\*/', '', loop_body, flags=re.DOTALL)
        
        # Pattern for array reductions
        # array_reduction_pattern = re.compile(r'(\w+)\[(\w+)\]\s*(?:(\+=|-=|\*=|&=|\|=|\^=)|(=)\s*\1\[(\w+)\]\s*([+\-*&|^]))')    
        array_reduction_pattern = re.compile(r'(\w+)(\[\w+\](?:\[\w+\])*)\s*(?:(\+=|-=|\*=|&=|\|=|\^=)|(=)\s*\1(\[\w+\](?:\[\w+\])*)\s*([+\-*&|^]))')
        for match in array_reduction_pattern.finditer(loop_body):
            var = match.group(1)
            indices = match.group(2)
            op = match.group(3) or match.group(6)
            
            # Extract all indices
            index_pattern = re.compile(r'\[(\w+)\]')
            indices = index_pattern.findall(indices)
            
            # Join indices for display
            index = ' '.join(indices)
            print(f"Array: {var}, Indices: {indices}, Operation: {op}")
            print(f"Array reduction: {var}[{index}] {op}")  # Debugging line
            
            # if index not in loop_vars:
            if all(index not in loop_vars for index in indices):
                array_size = None
                for line in loop_body.split('\n'):
                    # if f"for ({index}" in line:
                    if all(f"for ({index}" in line for index in individual_indices):
                        bounds_match = re.search(rf'{index}\s*=\s*(\w+|\d+)\s*;\s*{index}\s*<\s*(\w+|\d+)', line)
                        if bounds_match:
                            lower_bound = bounds_match.group(1)
                            array_size = bounds_match.group(2)
                            break
                
                if op in ['+', '+=']:
                    op = '+'
                elif op in ['-', '-=']:
                    op = '-'
                elif op in ['*', '*=']:
                    op = '*'
                elif op in ['&', '&=']:
                    op = '&'
                elif op in ['|', '|=']:
                    op = '|'
                elif op in ['^', '^=']:
                    op = '^'
            
                if array_size:
                    if lower_bound and lower_bound != '0':
                        reduction_vars[f"{var}[{lower_bound}:{array_size}]"] = op
                    else:
                        reduction_vars[f"{var}[:{array_size}]"] = op
                else:
                    reduction_vars[f"{var}[{index}]"] = op

        # Pattern for scalar reductions
        scalar_reduction_pattern = re.compile(r'(\w+)\s*(?:(\+=|-=|\*=|&=|\|=|\^=)|(=)\s*\1\s*([+\-*&|^]))')
        
        for match in scalar_reduction_pattern.finditer(loop_body):
            var = match.group(1)
            op = match.group(2) or match.group(4)

            if op in ['+', '+=']:
                op = '+'
            elif op in ['-', '-=']:
                op = '-'
            elif op in ['*', '*=']:
                op = '*'
            elif op in ['&', '&=']:
                op = '&'
            elif op in ['|', '|=']:
                op = '|'
            elif op in ['^', '^=']:
                op = '^'
            
            if var not in reduction_vars:
                reduction_vars[var] = op

        # print(f"Reduction variables: {reduction_vars}")  # Debugging line
        return reduction_vars


    def generate_reduction_clause(reduction_vars):
        grouped_reductions = {}
        for var, op in reduction_vars.items():
            if op not in grouped_reductions:
                grouped_reductions[op] = []
            grouped_reductions[op].append(var)
        
        reduction_clauses = []
        for op, vars in grouped_reductions.items():
            reduction_clauses.append(f"reduction({op}:{', '.join(vars)})")
        
        return " ".join(reduction_clauses)

    # Replace pragma omp for with pragma omp taskloop
    modified_code = ""
    last_end = 0
    
    for match in omp_for_pattern.finditer(code):
        # Append code before the match
        modified_code += code[last_end:match.start()]

        parsed_clauses = parse_omp_pragma(match)
    
        reduction_clause = parsed_clauses['reduction']
        reduction_vars = extract_reduction_variables(reduction_clause)
        private_clause = parsed_clauses['private']
        firstprivate_clause = parsed_clauses['firstprivate']
        lastprivate_clause = parsed_clauses['lastprivate']
        nowait_clause = parsed_clauses['nowait']
        schedule_clause = parsed_clauses['schedule']
        loop_control = parsed_clauses['loop_control']

        # print(f"Reduction: {reduction_clause}, Reduction variables: {reduction_vars}, Private: {private_clause}, "
        #   f"Firstprivate: {firstprivate_clause}, Lastprivate: {lastprivate_clause}, "
        #   f"Nowait: {nowait_clause}, Schedule: {schedule_clause}, Loop control: {loop_control}")
        
        # # Extract the reduction clause if present
        # reduction_clause = match.group(1)
        # print(f"Reduction clause: {reduction_clause}")  # Debugging line
        # reduction_vars = extract_reduction_variables(reduction_clause)
        
        # # nowait_clause = match.group(3)

        # # # Get loop control variables
        # # loop_control = match.group(5)
        # # Extract private, firstprivate, and lastprivate clauses
        # private_clause = match.group(5)      # private clause
        # firstprivate_clause = match.group(7) # firstprivate clause
        # lastprivate_clause = match.group(9)  # lastprivate clause

        # # Extract nowait clause if present
        # nowait_clause = match.group(10)

        # # Extract schedule(dynamic) clause if present
        # schedule_clause = match.group(11)

        # # Get loop control variables
        # loop_control = match.group(12)
        # # print(f"Loop control: {loop_control}")  # Debugging line


        # Extract the reduction clause if present
        # reduction_ops = match.group(2)
        # reduction_vars = match.group(3)
        # if reduction_ops and reduction_vars:
        #     reduction_clause = f"reduction({reduction_ops}:{reduction_vars})"
        # else:
        #     reduction_clause = None
        
        # # Extract private, firstprivate, and lastprivate clauses
        # private_clause = match.group(5)      # private clause
        # firstprivate_clause = match.group(7) # firstprivate clause
        # lastprivate_clause = match.group(9)  # lastprivate clause

        # # Extract nowait clause if present
        # nowait_clause = match.group(10)

        # # Extract schedule clause if present
        # schedule_clause = match.group(11)

        # # Get loop control variables
        # loop_control = match.group(12)

        # print(f"Reduction: {reduction_clause}, Private: {private_clause}, "
        #     f"Firstprivate: {firstprivate_clause}, Lastprivate: {lastprivate_clause}, "
        #     f"Nowait: {nowait_clause}, Schedule: {schedule_clause}, Loop control: {loop_control}")

        loop_vars = extract_loop_variables(loop_control) # Extract most uter loop variable
        # print(f"Loop variables: {loop_vars}")  # Debugging line
        loop_vars_for_private = extract_loop_variables_for_private(loop_control) # Extract most uter loop variable
        # print(f"Loop variables for private: {loop_vars_for_private}")  # Debugging line

        # Detect the loop body to find assigned and accessed variables
        loop_body_start = match.end()
        # print(f"Loop body start: {loop_body_start}")  # Debugging line

        # Use extract_full_loop_body to get the complete loop body
        loop_body = extract_full_loop_body(code, loop_body_start)
        # print(f"Loop body:\n{loop_body}")  # Debugging line 
            
        other_loop_vars = extract_other_loop_variables(loop_body)
        # print(f"Other loop variables: {other_loop_vars}")  # Debugging line
        other_loop_vars_for_private = extract_other_loop_variables_for_private(loop_body)
        # print(f"Other loop variables for private: {other_loop_vars_for_private}")  # Debugging line
        
        # Combine loop_vars and other_loop_vars while preserving order
        all_loop_vars = loop_vars + other_loop_vars
        # Print the combined list of loop variables in order
        # print(f"all loop variables: {all_loop_vars}")
        # print(f"other loop variables: {other_loop_vars}")

        all_loop_vars_for_private= loop_vars_for_private + other_loop_vars_for_private #set(loop_vars_for_private).union(other_loop_vars_for_private)
        # print(f"all loop variables for private: {all_loop_vars_for_private}")

        # Count nested loops to determine the collapse level
        # collapse_level = count_nested_loops(loop_body) + 1

        # Detect variables that are assigned or accessed in the loop body (version: considering collapse level)
        # assigned_vars, accessed_vars = detect_assigned_variables_with_collapse(loop_body, all_loop_vars, collapse_level)

        # Detect variables that are assigned or accessed in the loop body
        assigned_vars, accessed_vars = detect_assigned_variables(loop_body, loop_vars)
        # print(f"Assigned variables: {assigned_vars}")  # Debugging line
        # print(f"Accessed variables: {accessed_vars}")  # Debugging line
        
        # All loop variables, assigned variables, and accessed variables should be private
        private_vars = set(other_loop_vars_for_private).union(assigned_vars).union(accessed_vars)
        # print(f"Private variables: {private_vars}")  # Debugging line
        
        # Detect reduction variables in the loop body
        new_reduction_vars = detect_reduction_variables(loop_body, loop_vars)
        # print(f"New reduction variables: {new_reduction_vars}")  # Debugging line
        
        # Merge with existing reduction variables
        reduction_vars = {**new_reduction_vars, **reduction_vars}
        # print(f"Reduction variables: {reduction_vars}")  # Debugging line
        
        # Exclude reduction variables from the private variables
        private_vars.difference_update(reduction_vars)

        # Generate private clause string
        private_clause = ", ".join(private_vars)
        
        # Construct the pragma omp taskloop directive
        taskloop_clause = f"\t#pragma omp taskloop"

        if private_clause:
            taskloop_clause += f" private({private_clause})"
        
        # Generate reduction clause string
        reduction_clause = generate_reduction_clause(reduction_vars)
        
        if reduction_clause:
            taskloop_clause += f" {reduction_clause}"

        modified_code += f"#pragma omp single" + "\n"

        # Extract the indentation of the line containing the OpenMP directive
        indentation = code[:match.start()].splitlines()[-1][:len(code[:match.start()].splitlines()[-1]) - len(code[:match.start()].splitlines()[-1].lstrip())]

        modified_code += indentation + "{\n"
        modified_code += indentation + taskloop_clause + "\n"
        modified_code += f"{indentation}\tfor ({loop_control}) {{"

        # Append the loop body with proper indentation
        loop_body_lines = loop_body.splitlines()
        for i, line in enumerate(loop_body_lines):
            if line.strip():  # Avoid adding unnecessary blank lines
                modified_code += f"\n\t{line}"

        # Close the loop control block
        modified_code += f"\n{indentation}\t}}"

        # Close the single region
        modified_code += f"\n{indentation}}}"

        # Update the last end position
        # last_end = loop_body_start + len(loop_body)
        # Find the position of the next closing brace `}` after the loop body
        closing_brace_pos = code.find('}', loop_body_start + len(loop_body))
        if closing_brace_pos != -1:
            last_end = closing_brace_pos + 1
        else:
            last_end = loop_body_start + len(loop_body)
    
    # Append the remaining code
    modified_code += code[last_end:]
    
    return modified_code
    
# Example usage
if __name__ == "__main__":
    # Check if the input argument is provided
    if len(sys.argv) != 2:
        print("Usage: python For_to_Taskloop.py <input_filename>")
        sys.exit(1)

    # Get the input file name from the command-line argument
    input_filename = sys.argv[1]

    # Check if the file exists
    if not os.path.exists(input_filename):
        print(f"Error: {input_filename} does not exist.")
        sys.exit(1)

    # Read the input file
    with open(input_filename, "r") as f:
        code = f.read()

    # Convert the code
    converted_code = convert_for_to_taskloop(code)

    # Generate the output filename (X_taskloop.cpp)
    base_name, ext = os.path.splitext(input_filename)
    output_filename = f"{base_name}_taskloop{ext}"

    # Save the modified code to the new file
    with open(output_filename, "w") as f:
        f.write(converted_code)

    print(f"Converted code has been saved to {output_filename}")