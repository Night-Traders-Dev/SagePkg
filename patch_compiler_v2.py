import sys

with open('/root/sagelang/src/c/compiler.c', 'r') as f:
    content = f.read()

if '"platform",' not in content:
    content = content.replace('"substr",', '"substr", "platform",')

handling = '''    if (strcmp(callee_name, "platform") == 0) {
        if (call->arg_count == 0) {
            sb_append(&sb, "sage_platform_fn()");
        } else {
            compiler_builtin_arity_error(compiler, call, "platform", "usage: platform()", "0");
            sb_append(&sb, "sage_nil()");
        }
        free(callee_name);
        return sb_take(&sb);
    }'''

if 'strcmp(callee_name, "platform") == 0' not in content:
    content = content.replace('free(callee_name);\n        return sb_take(&sb);\n    }\n\n    if (strcmp(callee_name, "slice") == 0)', 
                              'free(callee_name);\n        return sb_take(&sb);\n    }\n\n' + handling + '\n\n    if (strcmp(callee_name, "slice") == 0)')

# implementation is already in prelude as sage_arch_fn, let's just alias it or add sage_platform_fn
# Actually sage_arch_fn returns "x86_64" or "aarch64".
# Let's add sage_platform_fn to prelude.

plat_fn = '''            "static SageValue sage_platform_fn(void) {\n"
            "    return sage_string(\"linux\");\n"
            "}\n"'''

if 'static SageValue sage_platform_fn' not in content:
    content = content.replace('static SageValue sage_input_fn', plat_fn + '\n            "static SageValue sage_input_fn')

with open('/root/sagelang/src/c/compiler.c', 'w') as f:
    f.write(content)
