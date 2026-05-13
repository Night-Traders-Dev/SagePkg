#!/usr/bin/env sage
import sys
import io
import string
import std.fmt as fmt

let ESC = chr(27)
let RESET = ESC + "[0m"
let BOLD = ESC + "[1m"
let GREEN = ESC + "[32m"
let BLUE = ESC + "[34m"
let CYAN = ESC + "[36m"
let RED = ESC + "[31m"

let HISTORY = []
let HISTORY_INDEX = 0

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

proc print_prompt():
    let p = GREEN + USER + "@" + HOST + RESET + " " + CYAN + BOLD + CWD + RESET + " 🌿 "
    # Write to temp file and cat it without newline
    io.writefile("/tmp/sage_prompt", p)
    sys.exec("cat /tmp/sage_prompt | tr -d '\\n'")

proc get_char():
    sys.exec("stty -icanon -echo && dd bs=1 count=1 2>/dev/null > /tmp/sage_key && stty icanon echo")
    let k = io.readfile("/tmp/sage_key")
    if k == nil or len(k) == 0:
        return nil
    return k[0]

proc print_line_raw(l):
    io.writefile("/tmp/sage_line", l)
    sys.exec("cat /tmp/sage_line | tr -d '\\n'")

proc sage_readline():
    let line = ""
    while true:
        let ch = get_char()
        if ch == nil:
            return nil
            
        let code = ord(ch)
        
        # Enter
        if code == 10 or code == 13:
            print ""
            return line
            
        # Backspace
        if code == 127 or code == 8:
            if len(line) > 0:
                line = string.substr(line, 0, len(line) - 1)
                # Move back, print space, move back
                sys.exec("printf '\\b \\b'")
            continue
            
        # Ctrl+L
        if code == 12:
            sys.exec("clear")
            print_prompt()
            print_line_raw(line)
            continue
            
        # Ctrl+D
        if code == 4:
            if len(line) == 0:
                return nil
            continue
            
        # Ctrl+C
        if code == 3:
            print "^C"
            return ""
            
        # Escape sequence (Arrows)
        if code == 27:
            let next1 = get_char()
            if next1 != nil and ord(next1) == 91:
                let next2 = get_char()
                # Up=A, Down=B
                if next2 != nil:
                    let dir = ord(next2)
                    if dir == 65: # Up
                        if HISTORY_INDEX > 0:
                            # Clear current line
                            for i in range(len(line)):
                                sys.exec("printf '\\b \\b'")
                            HISTORY_INDEX = HISTORY_INDEX - 1
                            line = HISTORY[HISTORY_INDEX]
                            print_line_raw(line)
                        continue
                    if dir == 66: # Down
                        if HISTORY_INDEX < len(HISTORY):
                            # Clear current line
                            for i in range(len(line)):
                                sys.exec("printf '\\b \\b'")
                            HISTORY_INDEX = HISTORY_INDEX + 1
                            if HISTORY_INDEX == len(HISTORY):
                                line = ""
                            else:
                                line = HISTORY[HISTORY_INDEX]
                            print_line_raw(line)
                        continue
            continue

        # Regular character
        if code >= 32 and code <= 126:
            line = line + ch
            print_line_raw(ch)
            
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
            
        # Add to history
        if len(HISTORY) == 0 or HISTORY[len(HISTORY)-1] != cmd_line:
            push(HISTORY, cmd_line)
            
        if cmd_line == "exit" or cmd_line == "quit":
            break
        
        if cmd_line == "clear":
            sys.exec("clear")
            continue
            
        if cmd_line == "help":
            print "SageShell - A fish clone in Sage"
            print "Built-in commands:"
            print "  cd <dir>   - Change directory"
            print "  clear      - Clear the screen"
            print "  help       - Show this help"
            print "  exit       - Exit the shell"
            print "Key combos:"
            print "  Ctrl+L     - Clear screen"
            print "  Ctrl+D     - Exit"
            print "  Up/Down    - History"
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
        
        # Execute external command
        let exec_cmd = "cd " + CWD + " && " + cmd_line
        let ret = sys.exec(exec_cmd)
        if ret != 0:
            pass

main()
