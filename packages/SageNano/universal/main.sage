import sys
from editor import Editor

proc main():
    let ed = Editor()
    let args = sys.args()
    if len(args) > 2:
        ed.load(args[2])
    ed.loop()

main()
