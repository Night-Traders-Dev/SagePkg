proc split_lines(s):
    let lines = []
    if s == nil:
        return lines
    let current = ""
    for i in range(len(s)):
        if s[i] == "\n":
            push(lines, current)
            current = ""
        else:
            current = current + s[i]
    if len(current) > 0:
        push(lines, current)
    return lines

let c = readfile("/etc/os-release")
let l = split_lines(c)
print "LINES: " + str(len(l))
