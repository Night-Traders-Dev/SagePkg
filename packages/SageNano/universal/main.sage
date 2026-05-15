import sys
import io
import string

let ESC = chr(27)
let RESET = ESC + "[0m"
let REVERSE = ESC + "[7m"

let lines = [""]
let cx = 0
let cy = 0
let filename = ""
let rows = 24
let cols = 80
let message = ""
let scroll_y = 0

proc trim(s):
    if s == nil: return ""
    let start_idx = 0
    while start_idx < len(s) and (s[start_idx] == " " or s[start_idx] == "\n" or s[start_idx] == "\r" or s[start_idx] == "\t"):
        start_idx = start_idx + 1
    let end_idx = len(s) - 1
    while end_idx >= start_idx and (s[end_idx] == " " or s[end_idx] == "\n" or s[end_idx] == "\r" or s[end_idx] == "\t"):
        end_idx = end_idx - 1
    if end_idx < start_idx: return ""
    let result = ""
    for i in range(end_idx - start_idx + 1):
        result = result + s[start_idx + i]
    return result

proc get_term_size():
    sys.exec("stty size 2>/dev/null > /tmp/sage_nano_size")
    let s = io.readfile("/tmp/sage_nano_size")
    if s == "" or s == nil:
        return [24, 80]
    let parts = split(s, " ")
    if len(parts) < 2:
        return [24, 80]
    let h = tonumber(trim(parts[0]))
    let w = tonumber(trim(parts[1]))
    if h == nil: h = 24
    if w == nil: w = 80
    return [h, w]

proc load_file(fname):
    if not io.exists(fname):
        filename = fname
        lines = [""]
        message = "New File"
        return
    let content = io.readfile(fname)
    if content == nil:
        message = "Error reading file"
        lines = [""]
        return
    filename = fname
    lines = split(content, chr(10))
    if len(lines) == 0:
        lines = [""]
    message = "Read " + str(len(lines)) + " lines"

proc save_file():
    if filename == "":
        message = "No filename"
        return
    let content = ""
    for i in range(len(lines)):
        content = content + lines[i]
        if i < len(lines) - 1:
            content = content + chr(10)
    io.writefile(filename, content)
    message = "Wrote " + str(len(lines)) + " lines"

proc draw_screen():
    let s = get_term_size()
    rows = s[0]
    cols = s[1]

    let out = ESC + "[?25l" # Hide cursor
    out = out + ESC + "[H"   # Move to top-left

    # Title bar
    let title = "  SageNano 1.0"
    let title_pad = (cols - len(title) - len(filename)) / 2 | 0
    if title_pad < 0: title_pad = 0
    let t_line = title
    for i in range(title_pad): t_line = t_line + " "
    if filename == "":
        t_line = t_line + "New Buffer"
    else:
        t_line = t_line + filename
    while len(t_line) < cols: t_line = t_line + " "
    out = out + REVERSE + string.substr(t_line, 0, cols) + RESET + "\r\n"

    # Adjust scroll
    if cy < scroll_y:
        scroll_y = cy
    if cy >= scroll_y + rows - 3:
        scroll_y = cy - (rows - 3) + 1
    if scroll_y < 0:
        scroll_y = 0

    # Text area
    for y in range(rows - 3):
        let file_y = scroll_y + y
        if file_y < len(lines):
            let l = lines[file_y]
            let d = string.substr(l, 0, cols)
            out = out + d
        out = out + ESC + "[K" # Clear rest of line
        out = out + "\r\n"

    # Status bar
    let msg_line = message
    while len(msg_line) < cols: msg_line = msg_line + " "
    out = out + REVERSE + string.substr(msg_line, 0, cols) + RESET + "\r\n"

    # Shortcut bar
    let shortcuts = "^X Exit  ^O Save"
    while len(shortcuts) < cols: shortcuts = shortcuts + " "
    out = out + REVERSE + string.substr(shortcuts, 0, cols) + RESET

    # Move cursor
    let screen_y = cy - scroll_y + 2
    let screen_x = cx + 1
    out = out + ESC + "[" + str(screen_y) + ";" + str(screen_x) + "H"
    out = out + ESC + "[?25h" # Show cursor

    # Print all at once
    io.writefile("/tmp/sage_nano_draw", out)
    sys.exec("cat /tmp/sage_nano_draw | tr -d '\\n'")

proc insert_char(ch):
    let l = lines[cy]
    lines[cy] = string.substr(l, 0, cx) + ch + string.substr(l, cx, len(l) - cx)
    cx = cx + 1

proc do_enter():
    let l = lines[cy]
    let left = string.substr(l, 0, cx)
    let right = string.substr(l, cx, len(l) - cx)
    lines[cy] = left
    
    let new_lines = []
    for i in range(cy + 1):
        push(new_lines, lines[i])
    push(new_lines, right)
    for i in range(len(lines) - cy - 1):
        push(new_lines, lines[cy + 1 + i])
    lines = new_lines
    cy = cy + 1
    cx = 0

proc do_backspace():
    if cx > 0:
        let l = lines[cy]
        lines[cy] = string.substr(l, 0, cx - 1) + string.substr(l, cx, len(l) - cx)
        cx = cx - 1
    else:
        if cy > 0:
            let l = lines[cy]
            let prev = lines[cy - 1]
            lines[cy - 1] = prev + l
            cx = len(prev)
            
            let new_lines = []
            for i in range(cy):
                push(new_lines, lines[i])
            for i in range(len(lines) - cy - 1):
                push(new_lines, lines[cy + 1 + i])
            lines = new_lines
            cy = cy - 1

proc editor_loop():
    sys.exec("stty raw -echo")
    
    while true:
        draw_screen()
        
        sys.exec("dd bs=1 count=1 2>/dev/null > /tmp/sage_nano_key")
        let k = io.readfile("/tmp/sage_nano_key")
        if k == nil or len(k) == 0:
            continue
            
        let ch = k[0]
        let code = ord(ch)
        
        message = ""

        if code == 24: # Ctrl-X
            break
        elif code == 15: # Ctrl-O
            save_file()
        elif code == 13 or code == 10: # Enter
            do_enter()
        elif code == 127 or code == 8: # Backspace
            do_backspace()
        elif code == 27: # Esc sequence
            sys.exec("dd bs=1 count=1 2>/dev/null > /tmp/sage_nano_key")
            let next1 = io.readfile("/tmp/sage_nano_key")
            if next1 != nil and ord(next1[0]) == 91:
                sys.exec("dd bs=1 count=1 2>/dev/null > /tmp/sage_nano_key")
                let next2 = io.readfile("/tmp/sage_nano_key")
                if next2 != nil:
                    let dir = ord(next2[0])
                    if dir == 65: # Up
                        if cy > 0: cy = cy - 1
                    elif dir == 66: # Down
                        if cy < len(lines) - 1: cy = cy + 1
                    elif dir == 67: # Right
                        if cx < len(lines[cy]): cx = cx + 1
                        else:
                            if cy < len(lines) - 1:
                                cy = cy + 1
                                cx = 0
                    elif dir == 68: # Left
                        if cx > 0: cx = cx - 1
                        else:
                            if cy > 0:
                                cy = cy - 1
                                cx = len(lines[cy])
            continue
        elif code >= 32 and code <= 126:
            insert_char(ch)
            
        if cy < 0: cy = 0
        if cy >= len(lines): cy = len(lines) - 1
        if cx < 0: cx = 0
        if cx > len(lines[cy]): cx = len(lines[cy])

    sys.exec("stty -raw echo")
    sys.exec("clear")

proc main():
    let args = sys.args()
    if len(args) > 2:
        load_file(args[2])
    else:
        message = "New Buffer"
        filename = ""
        
    sys.exec("printf '\\033[?1049h'")
    editor_loop()
    sys.exec("printf '\\033[?1049l'")

main()
