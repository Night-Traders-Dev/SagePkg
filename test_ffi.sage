let libc = ffi_open(nil)
let getenv_ptr = ffi_sym(libc, "getenv")
let user = ffi_call(getenv_ptr, ["USER"], "string")
print "User: " + str(user)
