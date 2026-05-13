let c = readfile("/etc/os-release")
if c == nil:
    print "NIL"
else:
    print "LEN: " + str(len(c))
