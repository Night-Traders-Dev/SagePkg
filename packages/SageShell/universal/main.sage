import sys
import io

let ESC = chr(27)
let RESET = ESC + "[0m"
let BOLD = ESC + "[1m"
let DIM = ESC + "[2m"
let ITALIC = ESC + "[3m"
let UNDERLINE = ESC + "[4m"

let GREEN = ESC + "[32m"
let BLUE = ESC + "[34m"
let CYAN = ESC + "[36m"
let RED = ESC + "[31m"
let GREY = ESC + "[90m"
let MAGENTA = ESC + "[35m"
let YELLOW = ESC + "[33m"
let WHITE = ESC + "[37m"

let BG_BLACK = ESC + "[40m"
let BG_RED = ESC + "[41m"
let BG_GREEN = ESC + "[42m"
let BG_YELLOW = ESC + "[43m"
let BG_BLUE = ESC + "[44m"
let BG_MAGENTA = ESC + "[45m"
let BG_CYAN = ESC + "[46m"
let BG_WHITE = ESC + "[47m"

let HISTORY = []
let HISTORY_INDEX = 0
let LAST_EXEC_TIME = 0.0

proc check_tty():
    return sys.exec("[ -t 0 ]") == 0

let IS_TTY = check_tty()

proc load_history():
    let home = sys.getenv("HOME")
    if home == nil:
        return
    let h_file = home + "/.sageshell_history"
    if io.exists(h_file):
        let content = io.readfile(h_file)
        if content != nil:
            let lines = split(content, chr(10))
            for i in range(len(lines)):
                let l = trim(lines[i])
                if len(l) > 0:
                    push(HISTORY, l)

proc save_history(line):
    let home = sys.getenv("HOME")
    if home == nil:
        return
    let h_file = home + "/.sageshell_history"
    io.appendfile(h_file, line + chr(10))

proc trim(s):
    if s == nil:
        return ""
    let start_idx = 0
    while start_idx < len(s) and (s[start_idx] == " " or s[start_idx] == "\n" or s[start_idx] == "\r" or s[start_idx] == "\t"):
        start_idx = start_idx + 1
    let end_idx = len(s) - 1
    while end_idx >= start_idx and (s[end_idx] == " " or s[end_idx] == "\n" or s[end_idx] == "\r" or s[end_idx] == "\t"):
        end_idx = end_idx - 1
    if end_idx < start_idx:
        return ""
    let result = ""
    for i in range(end_idx - start_idx + 1):
        result = result + s[start_idx + i]
    return result

proc starts_with(s, prefix):
    if len(s) < len(prefix):
        return false
    return s[0:len(prefix)] == prefix

proc str_contains(s, sub):
    return len(split(s, sub)) > 1

proc split_first(s, sep):
    for i in range(len(s)):
        if s[i] == sep:
            return [s[0:i], s[i+1:len(s)]]
    return [s, ""]

proc get_cwd():
    sys.exec("pwd > /tmp/sage_cwd")
    let cwd = io.readfile("/tmp/sage_cwd")
    if cwd == nil:
        return "/"
    let c = trim(cwd)
    let h = sys.getenv("HOME")
    if h != nil and starts_with(c, h):
        return "~" + c[len(h):len(c)]
    return c

proc get_user():
    let u = sys.getenv("USER")
    if u == nil:
        return "user"
    return u

let CWD = get_cwd()
let USER = get_user()

let ENV_PATH = sys.getenv("PATH")
let home = sys.getenv("HOME")

let STATE_FILE = "/tmp/sage_ui_state"
if home != nil:
    sys.exec("sage " + home + "/.sagepkg/packages/SageUtils/universal/ui_worker.sage >/dev/null 2>&1 &")

if ENV_PATH == nil or len(ENV_PATH) < 5:
    ENV_PATH = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

if home != nil:
    let s_bin = home + "/.sagepkg/bin"
    if not str_contains(ENV_PATH, s_bin):
        ENV_PATH = s_bin + ":" + ENV_PATH
    else:
        let parts = split(ENV_PATH, ":")
        let new_path = s_bin
        for i in range(len(parts)):
            if parts[i] != s_bin and len(parts[i]) > 0:
                new_path = new_path + ":" + parts[i]
        ENV_PATH = new_path

let sys_paths = ["/usr/local/bin", "/usr/bin", "/bin"]
for i in range(len(sys_paths)):
    if not str_contains(ENV_PATH, sys_paths[i]):
        ENV_PATH = ENV_PATH + ":" + sys_paths[i]

proc get_git_info():
    sys.exec("git rev-parse --is-inside-work-tree >/dev/null 2>&1 && echo 'yes' > /tmp/sage_git_is || echo 'no' > /tmp/sage_git_is")
    let is_git = io.readfile("/tmp/sage_git_is")
    if is_git == nil or trim(is_git) == "no":
        return ""
    sys.exec("git branch --show-current > /tmp/sage_git_branch 2>/dev/null")
    let b = io.readfile("/tmp/sage_git_branch")
    let branch_name = ""
    if b != nil:
        branch_name = trim(b)
    sys.exec("git status --porcelain > /tmp/sage_git_status 2>/dev/null")
    let st = io.readfile("/tmp/sage_git_status")
    let dirty = ""
    if st != nil and len(trim(st)) > 0:
        dirty = "*"
    if branch_name != "":
        return branch_name + dirty
    return ""

proc get_cached_state():
    let content = io.readfile(STATE_FILE)
    if content == nil:
        return ["N/A", "N/A", "24 80"]
    return split(content, "|")

proc draw_status_bar():
    if not IS_TTY:
        return
    let state = get_cached_state()
    let time = state[0]
    let temp = state[1]
    let size_parts = split(state[2], " ")
    let rows = tonumber(size_parts[0])
    let cols = tonumber(size_parts[1])

    let left = " 🐚 SageShell " + BOLD + CWD + RESET
    let mid = time
    let right = temp + " "

    let left_len = len(left) - 10 
    let mid_len = len(mid)
    let right_len = len(right)

    let pad_left_len = (cols / 2 | 0) - left_len - (mid_len / 2 | 0)
    if pad_left_len < 1:
        pad_left_len = 1

    let pad_right_len = cols - left_len - pad_left_len - mid_len - right_len
    if pad_right_len < 1:
        pad_right_len = 1

    let bar = ESC + "[44;37m" + left
    for i in range(pad_left_len):
        bar = bar + " "
    bar = bar + mid
    for i in range(pad_right_len):
        bar = bar + " "
    bar = bar + right + RESET

    let output = ESC + "[s" + ESC + "[" + str(rows) + ";1H" + bar + ESC + "[u"
    io.writefile("/dev/stdout", output)

proc print_prompt():
    let g = get_git_info()
    let time_str = ""
    if LAST_EXEC_TIME > 0.0:
        if LAST_EXEC_TIME > 1.0:
            time_str = " ⏳ " + str((LAST_EXEC_TIME * 10 | 0) / 10.0) + "s "
        else:
            time_str = " ⏳ " + str((LAST_EXEC_TIME * 1000 | 0)) + "ms "

    let p_user = BG_CYAN + BG_BLUE + BOLD + WHITE + " 🐚 " + USER + " " + RESET
    let p_cwd = BG_BLUE + BOLD + WHITE + " " + CWD + " " + RESET
    
    let p_git = ""
    if len(g) > 0:
        p_git = BG_MAGENTA + BOLD + WHITE + " 🌿 " + g + " " + RESET

    let p_time = ""
    if len(time_str) > 0:
        p_time = BG_BLACK + YELLOW + time_str + RESET

    let header = "\r" + ESC + "[K" + p_user + p_cwd + p_git + p_time + "\n"
    let prompt_sym = GREEN + BOLD + "❯" + RESET + " "
    io.writefile("/dev/stdout", header + prompt_sym)

proc print_line_raw(l):
    io.writefile("/dev/stdout", l)

proc is_builtin(cmd):
    if cmd == "exit" or cmd == "quit" or cmd == "help" or cmd == "cd" or cmd == "clear" or cmd == "export" or cmd == "env" or cmd == "version" or cmd == "source" or cmd == "reload" or cmd == "debug" or cmd == "history":
        return true
    return false

proc highlight(line):
    if len(line) == 0:
        return ""
    let result = ""
    let parts = []
    let current = ""
    let in_string = false
    for i in range(len(line)):
        let c = line[i]
        if c == " " and not in_string:
            if len(current) > 0:
            push(parts, current)
            push(parts, " ")
            current = ""
        elif c == chr(34) or c == chr(39):
            in_string = not in_string
            current = current + c
        else:
            current = current + c
    if len(current) > 0:
            push(parts, current)
    let cmd_found = false
    for i in range(len(parts)):
        let p = parts[i]
        if p == " ":
            result = result + " "
            continue
        if not cmd_found:
            cmd_found = true
            if is_builtin(p):
                    result = result + MAGENTA + BOLD + p + RESET
            elif command_exists(p):
                    result = result + GREEN + BOLD + p + RESET
            else: result = result + RED + p + RESET
        elif p[0] == "-":
                result = result + CYAN + p + RESET
        elif p[0] == chr(34) or p[0] == chr(39):
                result = result + YELLOW + p + RESET
        else: result = result + p
    return result

proc command_exists(cmd):
    if is_builtin(cmd):
        return true
    if len(cmd) == 0:
        return false
    if starts_with(cmd, "./") or starts_with(cmd, "/"):
        return (sys.exec("test -x " + cmd) == 0)
    let check_cmd = "PATH=" + ENV_PATH + " which " + cmd + " > /dev/null 2>&1"
    return sys.exec(check_cmd) == 0

proc find_suggestion(line):
    if len(line) == 0:
        return ""
    for i in range(len(HISTORY)):
        let h = HISTORY[len(HISTORY) - 1 - i]
        if starts_with(h, line):
            return h[len(line):len(h)]
    return ""

proc get_completions(line):
    let last_space = -1
    for i in range(len(line)):
        if line[i] == " ":
            last_space = i
    let word = line[last_space+1:len(line)]
    let results = []
    if last_space == -1:
        let builtins = ["exit", "quit", "help", "cd", "clear", "export", "env", "version"]
        for i in range(len(builtins)):
            if starts_with(builtins[i], word):
                    push(results, builtins[i])
        let path = ENV_PATH
        if path != nil:
            let dirs = split(path, ":")
            for i in range(len(dirs)):
                let d = dirs[i]
                if io.isdir(d):
                    sys.exec("ls -1 " + d + " 2>/dev/null > /tmp/sage_path_ls")
                    let content = io.readfile("/tmp/sage_path_ls")
                    if content != nil:
                        let files = split(content, chr(10))
                        for j in range(len(files)):
                            let f = trim(files[j])
                            if starts_with(f, word):
                                let exists = false
                                for k in range(len(results)):
                                    if results[k] == f:
                                            exists = true
                                if not exists:
                                        push(results, f)
    else:
        let dir = "."
        let prefix = word
        if str_contains(word, "/"):
            let last_slash = -1
            for i in range(len(word)):
                if word[i] == "/":
                        last_slash = i
            dir = word[0:last_slash+1]
            if dir == "":
                    dir = "/"
            prefix = word[last_slash+1:len(word)]
        sys.exec("ls -1 -F " + dir + " 2>/dev/null > /tmp/sage_ls")
        let content = io.readfile("/tmp/sage_ls")
        if content != nil:
            let files = split(content, chr(10))
            for i in range(len(files)):
                let f = trim(files[i])
                if starts_with(f, prefix):
                    if dir == "." or dir == "./":
                            push(results, f)
                    else: push(results, dir + f)
    return results

proc restore_terminal():
    sys.exec("stty icanon echo")

proc sage_readline():
    if not IS_TTY:
        sys.exec("read -r line_in && echo $line_in > /tmp/sage_in || echo 'EOF' > /tmp/sage_in")
        let res = trim(io.readfile("/tmp/sage_in"))
        if res == "EOF":
        return nil
        return res

    let line = ""
    let cursor = 0
    let suggestion = ""
    let h_search = ""
    let last_sec = ""
    let needs_redraw = true

    # Non-blocking raw mode
    sys.exec("stty -icanon -echo min 0 time 0")

    while true:
        let state = get_cached_state()
        let current_time = state[0]
        if current_time != last_sec:
            draw_status_bar()
            last_sec = current_time

        if needs_redraw:
            if cursor == len(line):
                    suggestion = find_suggestion(line)
            else: suggestion = ""

            io.writefile("/dev/stdout", "\r" + ESC + "[K")
            print_prompt()

            if cursor > 0:
                    print_line_raw(highlight(line[0:cursor]))
            let after = line[cursor:len(line)]
            if len(after) > 0:
                    print_line_raw(after)

            if cursor == len(line) and len(suggestion) > 0:
                print_line_raw(GREY + ITALIC + suggestion + RESET)
                for i in range(len(suggestion)):
                    io.writefile("/dev/stdout", "\b")

            if len(after) > 0:
                for i in range(len(after)):
                    io.writefile("/dev/stdout", "\b")

            needs_redraw = false

        sys.exec("dd bs=1 count=1 2>/dev/null > /tmp/sage_key")
        let k = io.readfile("/tmp/sage_key")
        if k == nil or len(k) == 0:
            sys.sleep(0.02) # 50 FPS idle
            continue

        needs_redraw = true
        let ch = k[0]
        let code = ord(ch)

        if code == 10 or code == 13:
            print ""
            restore_terminal()
            return line

        if code == 127 or code == 8:
            if cursor > 0:
                line = line[0:cursor-1] + line[cursor:len(line)]
                cursor = cursor - 1
                h_search = ""
            continue

        if code == 12:
            sys.exec("clear")
            continue

        if code == 4:
            if len(line) == 0:
                restore_terminal()
                return nil
            continue

        if code == 3:
            io.writefile("/dev/stdout", "\r" + ESC + "[K^C\r\n")
            restore_terminal()
            return ""

        if code == 1:
            cursor = 0
            continue

        if code == 5:
            cursor = len(line)
            continue

        if code == 9:
            if cursor == len(line) and len(suggestion) > 0:
                line = line + suggestion
                cursor = len(line)
            else:
                let comps = get_completions(line[0:cursor])
                if len(comps) == 1:
                    let last_space = -1
                    for i in range(len(line)):
                        if line[i] == " ":
                                last_space = i
                    line = line[0:last_space+1] + comps[0]
                    cursor = len(line)
                elif len(comps) > 1:
                    print ""
                    let comp_line = ""
                    for i in range(len(comps)):
                        comp_line = comp_line + comps[i] + "  "
                        if len(comp_line) > 60:
                            print comp_line
                            comp_line = ""
                    if len(comp_line) > 0:
                            print comp_line
            continue

        if code == 27:
            sys.exec("dd bs=1 count=1 2>/dev/null > /tmp/sage_key")
            let next1 = io.readfile("/tmp/sage_key")
            if next1 != nil and ord(next1[0]) == 91:
                sys.exec("dd bs=1 count=1 2>/dev/null > /tmp/sage_key")
                let next2 = io.readfile("/tmp/sage_key")
                if next2 != nil:
                    let d = ord(next2[0])
                    if d == 65:
                        if h_search == "":
                                h_search = line[0:cursor]
                        let idx = HISTORY_INDEX - 1
                        while idx >= 0:
                            if starts_with(HISTORY[idx], h_search):
                                HISTORY_INDEX = idx
                                line = HISTORY[idx]
                                cursor = len(line)
                                break
                            idx = idx - 1
                        continue
                    if d == 66:
                        if h_search == "":
                                h_search = line[0:cursor]
                        let idx = HISTORY_INDEX + 1
                        let found = false
                        while idx < len(HISTORY):
                            if starts_with(HISTORY[idx], h_search):
                                HISTORY_INDEX = idx
                                line = HISTORY[idx]
                                cursor = len(line)
                                found = true
                                break
                            idx = idx + 1
                        if not found:
                            line = h_search
                            cursor = len(line)
                            HISTORY_INDEX = len(HISTORY)
                        continue
                    if d == 67:
                        if cursor < len(line):
                                cursor = cursor + 1
                        elif len(suggestion) > 0:
                            line = line + suggestion
                            cursor = len(line)
                        continue
                    if d == 68:
                        if cursor > 0:
                                cursor = cursor - 1
                        continue
                    if d == 72:
                        cursor = 0
                        continue
                    if d == 70:
                        cursor = len(line)
                        continue
                    if d == 51:
                        sys.exec("dd bs=1 count=1 2>/dev/null > /dev/null")
                        if cursor < len(line):
                            line = line[0:cursor] + line[cursor+1:len(line)]
                        continue
                    if d == 49:
                        sys.exec("dd bs=2 count=1 2>/dev/null > /dev/null")
                        continue
            continue

        if code >= 32 and code <= 126:
            line = line[0:cursor] + ch + line[cursor:len(line)]
            cursor = cursor + 1
            h_search = ""

    restore_terminal()
    return line

proc process_command(cmd_line):
    if len(cmd_line) == 0:
        LAST_EXEC_TIME = 0.0
        return true

    let start_t = sys.clock()

    if cmd_line == "exit" or cmd_line == "quit":
            return false
    
    let is_handled = false
    if cmd_line == "clear":
        sys.exec("clear")
        is_handled = true
    elif cmd_line == "version":
        print BOLD + "SageShell" + RESET + " v2.0.0"
        print "Architecture: universal"
        is_handled = true
    elif cmd_line == "debug":
        print "CWD:  " + CWD
        print "USER: " + USER
        print "PATH: " + ENV_PATH
        print "HIST: " + str(len(HISTORY))
        is_handled = true
    elif cmd_line == "history":
        for i in range(len(HISTORY)):
            print " " + str(i + 1) + "  " + HISTORY[i]
        is_handled = true
    elif cmd_line == "reload":
        let h = sys.getenv("HOME")
        if h != nil:
            print "Reloading configuration..."
            process_command("source " + h + "/.sageshellrc")
        is_handled = true
    elif cmd_line == "help":
        print "SageShell - A modern, dynamic shell in SageLang"
        print "Built-ins: cd, clear, help, exit, export, env, version, source, reload, debug, history"
        print "Features: Git integration, execution timing, syntax highlighting, autosuggestions, tab completion"
        print "Keys: ←/→ move cursor  ↑/↓ history  ^A/^E home/end  Tab complete  ^C cancel  ^D EOF"
        is_handled = true
    elif starts_with(cmd_line, "source "):
        let parts = split_first(cmd_line, " ")
        let file = trim(parts[1])
        if io.exists(file):
            let content = io.readfile(file)
            let lines = split(content, chr(10))
            for i in range(len(lines)):
                process_command(trim(lines[i]))
        else:
            print "source: no such file: " + file
        is_handled = true
    elif cmd_line == "env":
        print "PATH=" + ENV_PATH
        is_handled = true
    elif starts_with(cmd_line, "export "):
        let parts = split_first(cmd_line, " ")
        let kv = trim(parts[1])
        if str_contains(kv, "="):
            let kv_parts = split_first(kv, "=")
            let key = trim(kv_parts[0])
            let val = trim(kv_parts[1])
            if key == "PATH":
                if str_contains(val, "$PATH"):
                    let v_parts = split(val, "$PATH")
                    val = v_parts[0] + ENV_PATH
                    if len(v_parts) > 1:
                            val = val + v_parts[1]
                ENV_PATH = val
        is_handled = true
    elif starts_with(cmd_line, "cd "):
        let parts = split_first(cmd_line, " ")
        let target = trim(parts[1])
        if len(target) == 0:
            let h = sys.getenv("HOME")
            if h != nil:
                    target = h
        let check_cmd = "cd " + CWD + " && cd '" + target + "' 2>/dev/null && pwd > /tmp/sage_cwd_new || echo 'ERROR' > /tmp/sage_cwd_new"
        sys.exec(check_cmd)
        let res = trim(io.readfile("/tmp/sage_cwd_new"))
        if res == "ERROR":
            print "cd: no such file or directory: " + target
        else:
            if len(res) > 0:
                CWD = res
                # Re-evaluate visual cwd
                let h = sys.getenv("HOME")
                if h != nil and starts_with(CWD, h):
                    CWD = "~" + CWD[len(h):len(CWD)]
        is_handled = true

    if not is_handled:
        let exec_cmd = "cd " + CWD + " && PATH=" + chr(34) + ENV_PATH + chr(34) + " " + cmd_line
        sys.exec(exec_cmd)
        # We need to refresh CWD in case the command was a script that modified state (though unlikely)
        CWD = get_cwd()

    LAST_EXEC_TIME = sys.clock() - start_t
    return true

proc main():
    load_history()
    print BG_CYAN + BG_BLUE + BOLD + WHITE + " Welcome to SageShell v2.0.0 " + RESET
    print ITALIC + "Type 'help' for commands, 'exit' to quit." + RESET

    let h = sys.getenv("HOME")
    if h != nil:
        let rc = h + "/.sageshellrc"
        if io.exists(rc):
            let content = io.readfile(rc)
            let lines = split(content, chr(10))
            for i in range(len(lines)):
                process_command(trim(lines[i]))

    while true:
        HISTORY_INDEX = len(HISTORY)
        let cmd_line = sage_readline()
        if cmd_line == nil:
            print "exit"
            break
        
        cmd_line = trim(cmd_line)
        if len(cmd_line) == 0:
            LAST_EXEC_TIME = 0.0
            continue

        if len(HISTORY) == 0 or HISTORY[len(HISTORY)-1] != cmd_line:
            push(HISTORY, cmd_line)
            save_history(cmd_line)

        if not process_command(cmd_line):
            break

main()
