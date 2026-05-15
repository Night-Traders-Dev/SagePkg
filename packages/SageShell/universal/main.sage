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
let MAGENTA = ESC + "[35m"
let YELLOW = ESC + "[33m"

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
    let c = trim(cwd)
    let h = sys.getenv("HOME")
    if h != nil and starts_with(c, h):
        return "~" + string.substr(c, len(h), len(c) - len(h))
    return c

proc get_user():
    let u = sys.getenv("USER")
    if u == nil:
        return "user"
    return u

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

let ENV_PATH = sys.getenv("PATH")
let home = sys.getenv("HOME")

if ENV_PATH == nil or len(ENV_PATH) < 5:
    ENV_PATH = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

if home != nil:
    let s_bin = home + "/.sagepkg/bin"
    if not string.contains(ENV_PATH, s_bin):
        ENV_PATH = s_bin + ":" + ENV_PATH
    else:
        # Move it to the front if it's already there
        let parts = split(ENV_PATH, ":")
        let new_path = s_bin
        for i in range(len(parts)):
            if parts[i] != s_bin and len(parts[i]) > 0:
                new_path = new_path + ":" + parts[i]
        ENV_PATH = new_path

# Ensure critical system paths
let sys_paths = ["/usr/local/bin", "/usr/bin", "/bin"]
for i in range(len(sys_paths)):
    if not string.contains(ENV_PATH, sys_paths[i]):
        ENV_PATH = ENV_PATH + ":" + sys_paths[i]

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
    
    let left = " 🐚 SageShell " + BOLD + CWD + RESET
    let mid = get_time()
    let right = get_temp_f() + " "
    
    let left_len = len(left) - 10 # Adjusted for emoji and bold
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
    let p = GREEN + USER + RESET + " " + CYAN + BOLD + CWD + RESET + " 🌿 "
    io.writefile("/tmp/sage_prompt", p)
    sys.exec("cat /tmp/sage_prompt | tr -d '\\n'")

proc print_line_raw(l):
    io.writefile("/tmp/sage_line", l)
    sys.exec("cat /tmp/sage_line | tr -d '\\n'")

proc is_builtin(cmd):
    if cmd == "exit" or cmd == "quit" or cmd == "help" or cmd == "cd" or cmd == "clear" or cmd == "export" or cmd == "env" or cmd == "version" or cmd == "source" or cmd == "reload":
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
        elif c == chr(34): # Quote
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
            
        if not cmd_found: # First non-space part is the command
            cmd_found = true
            if is_builtin(p):
                result = result + MAGENTA + p + RESET
            elif command_exists(p):
                result = result + GREEN + p + RESET
            else:
                result = result + RED + p + RESET
        elif p[0] == "-": # Flag
            result = result + CYAN + p + RESET
        elif p[0] == chr(34): # String
            result = result + YELLOW + p + RESET
        else:
            result = result + p
            
    return result

proc command_exists(cmd):
    if is_builtin(cmd):
        return true
    if len(cmd) == 0:
        return false
    if starts_with(cmd, "./") or starts_with(cmd, "/"):
        return (sys.exec("test -x " + cmd) == 0)
    
    let check_cmd = "PATH=" + ENV_PATH + " which " + cmd + " > /dev/null 2>&1"
    let res = sys.exec(check_cmd)
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
            
            print_line_raw(highlight(line))
                
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

proc process_command(cmd_line):
    if len(cmd_line) == 0:
        return true
        
    if cmd_line == "exit" or cmd_line == "quit":
        return false
    
    if cmd_line == "clear":
        sys.exec("clear")
        return true
        
    if cmd_line == "version":
        print "SageShell v1.3.5"
        return true
        
    if cmd_line == "help":
        print "SageShell - A fish clone in Sage"
        print "Built-in commands: cd, clear, help, exit, export, env, version, source"
        print "Fish features: Syntax Highlighting, Autosuggestions, Tab Completion, History Search, Real-time Status Bar"
        return true
        
    if starts_with(cmd_line, "source "):
        let parts = split_first(cmd_line, " ")
        let file = trim(parts[1])
        if io.exists(file):
            let content = io.readfile(file)
            let lines = split(content, chr(10))
            for i in range(len(lines)):
                process_command(trim(lines[i]))
        else:
            print "source: no such file: " + file
        return true
        
    if cmd_line == "env":
        print "PATH=" + ENV_PATH
        return true
        
    if starts_with(cmd_line, "export "):
        let parts = split_first(cmd_line, " ")
        let kv = trim(parts[1])
        if string.contains(kv, "="):
            let kv_parts = split_first(kv, "=")
            let key = trim(kv_parts[0])
            let val = trim(kv_parts[1])
            if key == "PATH":
                # Simple expansion for $PATH
                if string.contains(val, "$PATH"):
                    let v_parts = split(val, "$PATH")
                    val = v_parts[0] + ENV_PATH
                    if len(v_parts) > 1:
                        val = val + v_parts[1]
                ENV_PATH = val
        return true

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
        return true
    
    let exec_cmd = "cd " + CWD + " && PATH=" + chr(34) + ENV_PATH + chr(34) + " " + cmd_line
    sys.exec(exec_cmd)
    return true

proc main():
    print "Welcome to SageShell!"
    print "Type 'help' for commands, 'exit' to quit."
    
    # Load .sageshellrc
    let home = sys.getenv("HOME")
    if home != nil:
        let rc = home + "/.sageshellrc"
        if io.exists(rc):
            let content = io.readfile(rc)
            let lines = split(content, chr(10))
            for i in range(len(lines)):
                process_command(trim(lines[i]))

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
            
        if not process_command(cmd_line):
            break

main()
