#!/usr/bin/env python3
"""
transform_csv.py — Filter and rename columns from a BOM/parts CSV.
No third-party dependencies — stdlib only.

Usage:
    python transform_csv.py input.csv
    python transform_csv.py input.csv output.csv
"""

import csv
import os
import sys

COLUMN_MAP = {
    "Reference": "Reference",
    "Qty":       "Qty",
    "Value":     "Name",
    "Buy":       "Link",
    "Buy2":      "Additional",
    "Buy_ALT":   "Alternative",
}


def transform(input_path: str, output_path: str) -> None:
    with open(input_path, newline="", encoding="utf-8-sig") as f_in:
        reader = csv.DictReader(f_in)

        if reader.fieldnames is None:
            sys.exit("Error: CSV appears to be empty.")

        print(f"Loaded  : {input_path}")
        print(f"Columns : {list(reader.fieldnames)}\n")

        missing = [src for src in COLUMN_MAP if src not in reader.fieldnames]
        if missing:
            print(
                "Warning : the following source columns were NOT found and will be skipped:")
            for m in missing:
                print(f"           • {m}")
            print()

        available = {src: dst for src, dst in COLUMN_MAP.items()
                     if src in reader.fieldnames}
        out_fieldnames = list(available.values())

        with open(output_path, "w", newline="", encoding="utf-8") as f_out:
            writer = csv.DictWriter(f_out, fieldnames=out_fieldnames)
            writer.writeheader()

            row_count = 0
            for row in reader:
                writer.writerow({dst: row[src]
                                for src, dst in available.items()})
                row_count += 1

    print(
        f"Saved   : {output_path}  ({row_count} rows, {len(out_fieldnames)} columns)")
    print(f"Columns : {out_fieldnames}")


def main() -> None:
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    input_path = sys.argv[1]

    if len(sys.argv) >= 3:
        output_path = sys.argv[2]
    else:
        base, ext = os.path.splitext(input_path)
        output_path = f"{base}_transformed{ext}"

    transform(input_path, output_path)


if __name__ == "__main__":
    main()
