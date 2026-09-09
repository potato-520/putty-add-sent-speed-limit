#!/usr/bin/env python3
import os
import sys
import zipfile

def pack_assets(web_dir, bin_dir, out_zip):
    print(f"[pack_assets] Packing assets into {out_zip}...")
    count = 0
    with zipfile.ZipFile(out_zip, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        # 1. Add all files in web/ recursively
        for root, dirs, files in os.walk(web_dir):
            for f in sorted(files):
                full_path = os.path.join(root, f)
                rel_path = os.path.relpath(full_path, os.path.dirname(web_dir)).replace('\\', '/')
                z.write(full_path, rel_path)
                count += 1
                
        # 2. Add companion binaries (psftp.exe, plink.exe) directly under root of webview/
        for b in ['psftp.exe', 'plink.exe']:
            bp = os.path.join(bin_dir, b)
            if os.path.isfile(bp):
                z.write(bp, b)
                count += 1
            else:
                print(f"[pack_assets] Warning: companion binary not found: {bp}")

    size_mb = os.path.getsize(out_zip) / (1024 * 1024)
    print(f"[pack_assets] Successfully packed {count} files ({size_mb:.2f} MB) into {out_zip}")

if __name__ == '__main__':
    if len(sys.argv) < 4:
        print("Usage: pack_assets.py <web_dir> <bin_dir> <out_zip>")
        sys.exit(1)
    pack_assets(sys.argv[1], sys.argv[2], sys.argv[3])
