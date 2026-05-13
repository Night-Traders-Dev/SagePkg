import sys

with open('/root/sagelang/src/c/compiler.c', 'r') as f:
    content = f.read()

# 1. Add builtins to the list
content = content.replace('"clock", "input", "slice"', '"clock", "input", "exec", "readfile", "writefile", "getenv", "substr", "slice"')

# 2. Add handling in emit_builtin_call
input_handling = '''    if (strcmp(callee_name, "input") == 0) {
        if (call->arg_count == 0) {
            sb_append(&sb, "sage_input_fn(sage_nil())");
        } else if (call->arg_count == 1) {
            char* arg = emit_expr(compiler, call->args[0]);
            sb_appendf(&sb, "sage_input_fn(%s)", arg);
            free(arg);
        } else {
            compiler_builtin_arity_error(compiler, call, "input", "usage: input() or input(prompt)", "0 or 1");
            sb_append(&sb, "sage_nil()");
        }
        free(callee_name);
        return sb_take(&sb);
    }'''

new_handling = input_handling + '''
    if (strcmp(callee_name, "exec") == 0) {
        if (call->arg_count == 1) {
            char* arg = emit_expr(compiler, call->args[0]);
            sb_appendf(&sb, "sage_exec(%s)", arg);
            free(arg);
        } else {
            compiler_builtin_arity_error(compiler, call, "exec", "usage: exec(cmd)", "1");
            sb_append(&sb, "sage_nil()");
        }
        free(callee_name);
        return sb_take(&sb);
    }
    if (strcmp(callee_name, "readfile") == 0) {
        if (call->arg_count == 1) {
            char* arg = emit_expr(compiler, call->args[0]);
            sb_appendf(&sb, "sage_readfile(%s)", arg);
            free(arg);
        } else {
            compiler_builtin_arity_error(compiler, call, "readfile", "usage: readfile(path)", "1");
            sb_append(&sb, "sage_nil()");
        }
        free(callee_name);
        return sb_take(&sb);
    }
    if (strcmp(callee_name, "writefile") == 0) {
        if (call->arg_count == 2) {
            char* arg1 = emit_expr(compiler, call->args[0]);
            char* arg2 = emit_expr(compiler, call->args[1]);
            sb_appendf(&sb, "sage_writefile(%s, %s)", arg1, arg2);
            free(arg1); free(arg2);
        } else {
            compiler_builtin_arity_error(compiler, call, "writefile", "usage: writefile(path, content)", "2");
            sb_append(&sb, "sage_nil()");
        }
        free(callee_name);
        return sb_take(&sb);
    }
    if (strcmp(callee_name, "getenv") == 0) {
        if (call->arg_count == 1) {
            char* arg = emit_expr(compiler, call->args[0]);
            sb_appendf(&sb, "sage_getenv(%s)", arg);
            free(arg);
        } else {
            compiler_builtin_arity_error(compiler, call, "getenv", "usage: getenv(name)", "1");
            sb_append(&sb, "sage_nil()");
        }
        free(callee_name);
        return sb_take(&sb);
    }
    if (strcmp(callee_name, "substr") == 0) {
        if (call->arg_count == 3) {
            char* arg1 = emit_expr(compiler, call->args[0]);
            char* arg2 = emit_expr(compiler, call->args[1]);
            char* arg3 = emit_expr(compiler, call->args[2]);
            sb_appendf(&sb, "sage_substr(%s, %s, %s)", arg1, arg2, arg3);
            free(arg1); free(arg2); free(arg3);
        } else {
            compiler_builtin_arity_error(compiler, call, "substr", "usage: substr(s, start, len)", "3");
            sb_append(&sb, "sage_nil()");
        }
        free(callee_name);
        return sb_take(&sb);
    }'''

content = content.replace(input_handling, new_handling)

# 3. Add C implementations in prelude
input_fn = '''            "    return sage_string(buf);\n"
            "}\n"'''

new_fns = input_fn + '''
            "static SageValue sage_exec(SageValue cmd) {\n"
            "    if (cmd.type != SAGE_TAG_STRING) return sage_number(-1);\n"
            "    int res = system(cmd.as.string);\n"
            "    return sage_number((double)res);\n"
            "}\n"
            "static SageValue sage_readfile(SageValue path) {\n"
            "    if (path.type != SAGE_TAG_STRING) return sage_nil();\n"
            "    FILE* f = fopen(path.as.string, \"rb\");\n"
            "    if (!f) return sage_nil();\n"
            "    fseek(f, 0, SEEK_END);\n"
            "    long len = ftell(f);\n"
            "    fseek(f, 0, SEEK_SET);\n"
            "    char* buf = malloc(len + 1);\n"
            "    fread(buf, 1, len, f);\n"
            "    buf[len] = '\\0';\n"
            "    fclose(f);\n"
            "    SageValue res = sage_string(buf);\n"
            "    free(buf);\n"
            "    return res;\n"
            "}\n"
            "static SageValue sage_writefile(SageValue path, SageValue content) {\n"
            "    if (path.type != SAGE_TAG_STRING || content.type != SAGE_TAG_STRING) return sage_nil();\n"
            "    FILE* f = fopen(path.as.string, \"wb\");\n"
            "    if (!f) return sage_nil();\n"
            "    fputs(content.as.string, f);\n"
            "    fclose(f);\n"
            "    return sage_nil();\n"
            "}\n"
            "static SageValue sage_getenv(SageValue name) {\n"
            "    if (name.type != SAGE_TAG_STRING) return sage_nil();\n"
            "    char* val = getenv(name.as.string);\n"
            "    if (!val) return sage_nil();\n"
            "    return sage_string(val);\n"
            "}\n"
            "static SageValue sage_substr(SageValue s, SageValue start, SageValue len) {\n"
            "    if (s.type != SAGE_TAG_STRING || start.type != SAGE_TAG_NUMBER || len.type != SAGE_TAG_NUMBER) return sage_string(\"\");\n"
            "    int st = (int)start.as.number;\n"
            "    int l = (int)len.as.number;\n"
            "    int slen = strlen(s.as.string);\n"
            "    if (st < 0) st = 0;\n"
            "    if (st > slen) st = slen;\n"
            "    if (st + l > slen) l = slen - st;\n"
            "    if (l < 0) l = 0;\n"
            "    char* res = malloc(l + 1);\n"
            "    strncpy(res, s.as.string + st, l);\n"
            "    res[l] = '\\0';\n"
            "    SageValue val = sage_string(res);\n"
            "    free(res);\n"
            "    return val;\n"
            "}\n"'''

content = content.replace(input_fn, new_fns)

with open('/root/sagelang/src/c/compiler.c', 'w') as f:
    f.write(content)
