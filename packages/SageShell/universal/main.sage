import sys
import io
import string

let ESC = chr(27)
let RESET = ESC + "[0m"
let BOLD = ESC + "[1m"
let GREEN = ESC + "[32m"
let BLUE = ESC + "[34m"
let CYAN = ESC + "[36m"
let RED = ESC + "[31m"
let GREY = ESC + "[90m"

let HISTORY = []
let HISTORY_INDEX = 0
let IS_TTY = (sys.exec("[ -t 0 ]") == 0)

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

proc get_cwd():
    sys.exec("pwd > /tmp/sage_cwd")
    let cwd = io.readfile("/tmp/sage_cwd")
    if cwd == nil:
        return "/"
    return trim(cwd)

proc get_user():
    let u = sys.getenv("USER")
    if u == nil:
        return "user"
    return u

proc get_host():
    let h = io.readfile("/etc/hostname")
    if h == nil:
        return "sage"
    return trim(h)

proc starts_with(s, prefix):
    if len(s) < len(prefix):
        return false
    return string.substr(s, 0, len(prefix)) == prefix

proc split_first(s, sep):
    for i in range(len(s)):
        if s[i] == sep:
            return [string.substr(s, 0, i), string.substr(s, i + 1, len(s) - i - 1)]
    return [s, ""]

let CWD = get_cwd()
let USER = get_user()
let HOST = get_host()

proc get_temp_f():
    sys.exec("cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null > /tmp/sage_temp")
    let t = io.readfile("/tmp/sage_temp")
    if t == nil or len(t) == 0:
        return "N/A"
    let mc = tonumber(trim(t))
    if mc == nil:
        return "N/A"
    let c = mc / 1000
    let f = (c * 9 / 5) + 32
    return str(f | 0) + "°F"

proc get_time():
    sys.exec("date +%H:%M:%S > /tmp/sage_time")
    return trim(io.readfile("/tmp/sage_time"))

proc get_term_size():
    sys.exec("stty size 2>/dev/null > /tmp/sage_size")
    let s = trim(io.readfile("/tmp/sage_size"))
    if s == "" or s == nil:
        return [24, 80]
    let parts = split(s, " ")
    if len(parts) < 2:
        return [24, 80]
    return [tonumber(parts[0]), tonumber(parts[1])]

proc draw_status_bar():
    if not IS_TTY:
        return
    let size = get_term_size()
    let rows = size[0]
    let cols = size[1]
    
    let left = " 🐚 SageShell"
    let mid = get_time()
    let right = get_temp_f() + " "
    
    let left_len = len(left) - 1 # Adjusted for emoji
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
    
    # Save cursor, move to bottom, print bar, restore cursor
    let cmd = "printf '" + ESC + "[s" + ESC + "[" + str(rows) + ";1H" + bar + ESC + "[u'"
    sys.exec(cmd)

proc print_prompt():
    let p = GREEN + USER + "@" + HOST + RESET + " " + CYAN + BOLD + CWD + RESET + " 🌿 "
    io.writefile("/tmp/sage_prompt", p)
    sys.exec("cat /tmp/sage_prompt | tr -d '\\n'")

proc print_line_raw(l):
    io.writefile("/tmp/sage_line", l)
    sys.exec("cat /tmp/sage_line | tr -d '\\n'")

proc is_builtin(cmd):
    if cmd == "exit" or cmd == "quit" or cmd == "help" or cmd == "cd" or cmd == "clear":
        return true
    return false

proc command_exists(cmd):
    if is_builtin(cmd):
        return true
    if len(cmd) == 0:
        return false
    if starts_with(cmd, "./") or starts_with(cmd, "/"):
        return (sys.exec("test -x " + cmd) == 0)
    let res = sys.exec("which " + cmd + " > /dev/null 2>&1")
    return res == 0

proc find_suggestion(line):
    if len(line) == 0:
        return ""
    for i in range(len(HISTORY)):
        let h = HISTORY[len(HISTORY) - 1 - i]
        if starts_with(h, line):
            return string.substr(h, len(line), len(h) - len(line))
    return ""

proc get_completions(line):
    let last_space = -1
    for i in range(len(line)):
        if line[i] == " ":
            last_space = i
    let word = string.substr(line, last_space + 1, len(line) - last_space - 1)
    
    let results = []
    
    if last_space == -1:
        let builtins = ["exit", "quit", "help", "cd", "clear"]
        for i in range(len(builtins)):
            if starts_with(builtins[i], word):
                push(results, builtins[i])
        
        let path = sys.getenv("PATH")
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
        if string.contains(word, "/"):
            let last_slash = -1
            for i in range(len(word)):
                if word[i] == "/":
                    last_slash = i
            dir = string.substr(word, 0, last_slash + 1)
            if dir == "":
                dir = "/"
            prefix = string.substr(word, last_slash + 1, len(word) - last_slash - 1)
        
        sys.exec("ls -1 -F " + dir + " 2>/dev/null > /tmp/sage_ls")
        let content = io.readfile("/tmp/sage_ls")
        if content != nil:
            let files = split(content, chr(10))
            for i in range(len(files)):
                let f = trim(files[i])
                if starts_with(f, prefix):
                    if dir == "." or dir == "./":
                        push(results, f)
                    else:
                        push(results, dir + f)
    return results

proc sage_readline():
    if not IS_TTY:
        sys.exec("read -r line_in && echo $line_in > /tmp/sage_in || echo 'EOF' > /tmp/sage_in")
        let res = trim(io.readfile("/tmp/sage_in"))
        if res == "EOF":
            return nil
        return res

    let line = ""
    let suggestion = ""
    let h_search = ""
    let last_sec = ""
    let needs_redraw = true
    
    sys.exec("stty -icanon -echo min 0 time 2")
    
    while true:
        let current_time = get_time()
        if current_time != last_sec:
            draw_status_bar()
            last_sec = current_time
            
        if needs_redraw:
            suggestion = find_suggestion(line)
            sys.exec("printf '\\r" + ESC + "[K'")
            print_prompt()
            
            let parts = split_first(line, " ")
            let cmd = parts[0]
            if len(cmd) > 0:
                if command_exists(cmd):
                    print_line_raw(GREEN + cmd + RESET + string.substr(line, len(cmd), len(line) - len(cmd)))
                else:
                    print_line_raw(RED + cmd + RESET + string.substr(line, len(cmd), len(line) - len(cmd)))
            else:
                print_line_raw(line)
                
            if len(suggestion) > 0:
                print_line_raw(GREY + suggestion + RESET)
                for i in range(len(suggestion)):
                    sys.exec("printf '\\b'")
            needs_redraw = false
        
        sys.exec("dd bs=1 count=1 2>/dev/null > /tmp/sage_key")
        let k = io.readfile("/tmp/sage_key")
        if k == nil or len(k) == 0:
            continue
        
        needs_redraw = true
        let ch = k[0]
        let code = ord(ch)
        
        if code == 10 or code == 13:
            print ""
            sys.exec("stty icanon echo")
            return line
            
        if code == 127 or code == 8:
            if len(line) > 0:
                line = string.substr(line, 0, len(line) - 1)
                h_search = ""
            continue
            
        if code == 12:
            sys.exec("clear")
            continue
            
        if code == 4:
            if len(line) == 0:
                sys.exec("stty icanon echo")
                return nil
            continue
            
        if code == 3:
            print "^C"
            sys.exec("stty icanon echo")
            return ""
            
        if code == 9: # Tab
            if len(suggestion) > 0:
                line = line + suggestion
            else:
                let comps = get_completions(line)
                if len(comps) == 1:
                    let last_space = -1
                    for i in range(len(line)):
                        if line[i] == " ":
                            last_space = i
                    line = string.substr(line, 0, last_space + 1) + comps[0]
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
                    let dir = ord(next2[0])
                    if dir == 65: # Up
                        if h_search == "":
                            h_search = line
                        let found = false
                        let idx = HISTORY_INDEX - 1
                        while idx >= 0:
                            if starts_with(HISTORY[idx], h_search):
                                HISTORY_INDEX = idx
                                line = HISTORY[idx]
                                found = true
                                break
                            idx = idx - 1
                        continue
                    if dir == 66: # Down
                        if h_search == "":
                            h_search = line
                        let found = false
                        let idx = HISTORY_INDEX + 1
                        while idx < len(HISTORY):
                            if starts_with(HISTORY[idx], h_search):
                                HISTORY_INDEX = idx
                                line = HISTORY[idx]
                                found = true
                                break
                            idx = idx + 1
                        if not found:
                            line = h_search
                            HISTORY_INDEX = len(HISTORY)
                        continue
                    if dir == 67: # Right
                        if len(suggestion) > 0:
                            line = line + suggestion
                        continue
            continue

        if code >= 32 and code <= 126:
            line = line + ch
            h_search = ""
            
    sys.exec("stty icanon echo")
    return line

proc main():
    print "Welcome to SageShell!"
    print "Type 'help' for commands, 'exit' to quit."
    
    while true:
        print_prompt()
        HISTORY_INDEX = len(HISTORY)
        let cmd_line = sage_readline()
        if cmd_line == nil:
            print "exit"
            break
        
        cmd_line = trim(cmd_line)
        if len(cmd_line) == 0:
            continue
            
        if len(HISTORY) == 0 or HISTORY[len(HISTORY)-1] != cmd_line:
            push(HISTORY, cmd_line)
            
        if cmd_line == "exit" or cmd_line == "quit":
            break
        
        if cmd_line == "clear":
            sys.exec("clear")
            continue
            
        if cmd_line == "help":
            print "SageShell - A fish clone in Sage"
            print "Built-in commands: cd, clear, help, exit"
            print "Fish features: Syntax Highlighting, Autosuggestions, Tab Completion, History Search, Real-time Status Bar"
            continue
            
        if starts_with(cmd_line, "cd "):
            let parts = split_first(cmd_line, " ")
            let target = trim(parts[1])
            if len(target) == 0:
                let home = sys.getenv("HOME")
                if home != nil:
                    target = home
            
            let check_cmd = "cd " + CWD + " && cd '" + target + "' 2>/dev/null && pwd > /tmp/sage_cwd_new || echo 'ERROR' > /tmp/sage_cwd_new"
            sys.exec(check_cmd)
            let res = trim(io.readfile("/tmp/sage_cwd_new"))
            if res == "ERROR":
                print "cd: no such file or directory: " + target
            else:
                if len(res) > 0:
                    CWD = res
            continue
        
        let exec_cmd = "cd " + CWD + " && " + cmd_line
        sys.exec(exec_cmd)

main()
