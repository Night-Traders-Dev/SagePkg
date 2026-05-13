import sys

with open('/root/sagelang/src/c/compiler.c', 'r') as f:
    content = f.read()

# Add prototypes to emit_runtime_prelude
marker = 'static void emit_runtime_prelude(Compiler* compiler, COMPILER_TARGET target) {'
prototypes = '''
    if (target == COMPILER_TARGET_HOST) {
        fputs(
            "static SageValue sage_exec(SageValue cmd);\n"
            "static SageValue sage_readfile(SageValue path);\n"
            "static SageValue sage_writefile(SageValue path, SageValue content);\n"
            "static SageValue sage_getenv(SageValue name);\n"
            "static SageValue sage_substr(SageValue s, SageValue start, SageValue len);\n"
            "static SageValue sage_platform_fn(void);\n",
            compiler->out
        );
    }'''

if 'static SageValue sage_exec' not in content:
    content = content.replace(marker, marker + prototypes)

with open('/root/sagelang/src/c/compiler.c', 'w') as f:
    f.write(content)
