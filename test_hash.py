import sys

def fnv1a64(data: bytes) -> int:
    OFFSET = 14695981039346656037
    PRIME = 1099511628211
    MASK = (1 << 64) - 1
    h = OFFSET
    for b in data:
        h ^= b
        h = (h * PRIME) & MASK
    return h

path = sys.argv[1]
with open(path, 'rb') as f:
    data = f.read()

h = fnv1a64(data)
print(f"Hash: {h:016x}")
print(f"Size: {len(data)}")
