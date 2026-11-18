import sys
sys.path.insert(0, "tools/assets")
import crop

src, x0, y0, x1, y1, scale, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6]), sys.argv[7]
w, h, rows = crop.load("tools/assets/out/" + src)
region = [r[x0 * 3:x1 * 3] for r in rows[y0:y1]]
out_rows = []
for r in region:
    line = bytearray()
    for px in range(0, len(r), 3):
        line += r[px:px + 3] * scale
    for _ in range(scale):
        out_rows.append(bytes(line))
crop.save(out, (x1 - x0) * scale, (y1 - y0) * scale, out_rows)
print("wrote %s (%dx%d)" % (out, (x1 - x0) * scale, (y1 - y0) * scale))
