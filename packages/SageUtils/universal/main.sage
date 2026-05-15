import sys
import io
import string

# ANSI Colors
let ESC = chr(27)
let RESET = ESC + "[0m"
let BOLD = ESC + "[1m"
let GREEN = ESC + "[32m"
let BLUE = ESC + "[34m"
let RED = ESC + "[31m"
let YELLOW = ESC + "[33m"

proc ui_error(msg):
    print RED + BOLD + "error:" + RESET + " " + msg

proc sls(args):
    let dir = "."
    if len(args) > 1:
        dir = args[1]
    
    sys.exec("ls -F " + dir + " 2>/tmp/sage_ls_err > /tmp/sage_ls_out")
    let err = io.readfile("/tmp/sage_ls_err")
    if err != nil and len(err) > 0:
        ui_error(err)
        return
    
    let out = io.readfile("/tmp/sage_ls_out")
    if out != nil:
        print out

proc scat(args):
    if len(args) < 2:
        ui_error("missing file operand")
        return
    
    for i in range(len(args) - 1):
        let fname = args[i + 1]
        let content = io.readfile(fname)
        if content == nil:
            ui_error("could not read " + fname)
        else:
            print content

proc secho(args):
    let line = ""
    for i in range(len(args) - 1):
        line = line + args[i + 1]
        if i < len(args) - 2:
            line = line + " "
    print line

proc stouch(args):
    if len(args) < 2:
        ui_error("missing file operand")
        return
    for i in range(len(args) - 1):
        sys.exec("touch " + args[i + 1])

proc smkdir(args):
    if len(args) < 2:
        ui_error("missing directory operand")
        return
    for i in range(len(args) - 1):
        io.mkdir(args[i + 1])

proc srm(args):
    if len(args) < 2:
        ui_error("missing operand")
        return
    for i in range(len(args) - 1):
        sys.exec("rm -rf " + args[i + 1])

proc main():
    let args = sys.args()
    if len(args) < 2:
        return

    let script_path = args[1]
    let cmd = ""
    if string.contains(script_path, "/"):
        let parts = split(script_path, "/")
        cmd = parts[len(parts) - 1]
    else:
        cmd = script_path

    # Route based on name
    if cmd == "sls":
        sls(args)
    elif cmd == "scat":
        scat(args)
    elif cmd == "secho":
        secho(args)
    elif cmd == "stouch":
        stouch(args)
    elif cmd == "smkdir":
        smkdir(args)
    elif cmd == "srm":
        srm(args)
    elif cmd == "main.sage" or cmd == "SageUtils":
        if len(args) < 3:
            print BOLD + "SageUtils" + RESET + " - Common terminal utilities in Sage"
            print "Usage: SageUtils <command> [args]"
            print "Commands: sls, scat, secho, stouch, smkdir, srm"
            return
        
        let subcmd = args[2]
        # Shift args for subcommands
        let new_args = []
        push(new_args, args[0])
        push(new_args, args[1])
        for i in range(len(args) - 3):
            push(new_args, args[i + 3])
            
        if subcmd == "sls": sls(new_args)
        elif subcmd == "scat": scat(new_args)
        elif subcmd == "secho": secho(new_args)
        elif subcmd == "stouch": stouch(new_args)
        elif subcmd == "smkdir": smkdir(new_args)
        elif subcmd == "srm": srm(new_args)
        else: ui_error("unknown command: " + subcmd)
    else:
        ui_error("unknown entry point: " + cmd)

main()
