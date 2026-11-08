import sys
path = sys.argv[1]
data = open(path, 'rb').read()
fixed = data.replace(b'\x00', b'\\x00')
open(path, 'wb').write(fixed)
print("replaced", data.count(b'\x00'), "NUL byte(s)")
