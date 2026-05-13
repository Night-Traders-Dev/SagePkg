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
    let p = GREEN + USER + "@" + HOST + RESET + " " + CYAN + BOLD + CWD + RESET + " > "
    # Write to temp file and cat it without newline
    io.writefile("/tmp/sage_prompt", p)
    sys.exec("cat /tmp/sage_prompt | tr -d '\n'")

proc main():
    print "Welcome to SageShell!"
    print "Type 'help' for commands, 'exit' to quit."
    
    while true:
        print_prompt()
        let cmd_raw = input()
        if cmd_raw == nil:
            break
        
        let cmd_line = trim(cmd_raw)
        if len(cmd_line) == 0:
            continue
            
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
            continue
            
        if starts_with(cmd_line, "cd "):
            let parts = split_first(cmd_line, " ")
            let target = trim(parts[1])
            if len(target) == 0:
                let home = sys.getenv("HOME")
                if home != nil:
                    target = home
            
            # To actually check if we can cd and get the new path,
            # we execute `cd target && pwd` in a subshell and read the output.
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
            pass # The command itself will likely print the error

main()
