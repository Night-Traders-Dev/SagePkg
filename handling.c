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
    }
    if (strcmp(callee_name, "platform") == 0) {
        if (call->arg_count == 0) {
            sb_append(&sb, "sage_platform_fn()");
        } else {
            compiler_builtin_arity_error(compiler, call, "platform", "usage: platform()", "0");
            sb_append(&sb, "sage_nil()");
        }
        free(callee_name);
        return sb_take(&sb);
    }
