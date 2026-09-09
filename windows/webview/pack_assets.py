#!/usr/bin/env python3
import os
import sys
import zipfile

def pack_assets(web_dir, out_zip):
    print(f"[pack_assets] Packing assets into {out_zip}...")
    count = 0
    with zipfile.ZipFile(out_zip, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        # Add all files in web/ recursively
        for root, dirs, files in os.walk(web_dir):
            for f in sorted(files):
                full_path = os.path.join(root, f)
                rel_path = os.path.relpath(full_path, os.path.dirname(web_dir)).replace('\\', '/')
                z.write(full_path, rel_path)
                count += 1

    size_mb = os.path.getsize(out_zip) / (1024 * 1024)
    print(f"[pack_assets] Successfully packed {count} files ({size_mb:.2f} MB) into {out_zip}")

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("Usage: pack_assets.py <web_dir> <out_zip> [ignored_bin_dir]")
        sys.exit(1)
    if len(sys.argv) == 3:
        pack_assets(sys.argv[1], sys.argv[2])
    else:
        pack_assets(sys.argv[1], sys.argv[3])

