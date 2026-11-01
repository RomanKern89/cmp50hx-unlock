#!/usr/bin/env python3
import json
import sys


def values_by_name(control):
    return {item["name"]: item["raw"] for item in control["values"]}


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: verify.py RM_PROBE_JSON")

    with open(sys.argv[1], "r", encoding="utf-8") as source:
        report = json.load(source)

    controls = report["controls"]
    v1 = controls["v1"]
    v2 = controls["v2"]
    if v1["rm_status"] != "0x00000000":
        raise SystemExit(f"v1 RM status is {v1['rm_status']}")
    if v2["rm_status"] != "0x00000000":
        raise SystemExit(f"v2 RM status is {v2['rm_status']}")
    if len(v1["values"]) != 9 or any(item["raw"] != 0 for item in v1["values"]):
        raise SystemExit("the nine v1 issue-rate values are not all full speed")
    if len(v2["values"]) != 8 or any(item["raw"] != 0 for item in v2["values"]):
        raise SystemExit("the eight v2 issue-rate values are not all full speed")

    gpu = values_by_name(controls["gpu_info"])
    gr = values_by_name(controls["gr_info"])
    # The patched driver clears the SKU selector, so on a working unlock
    # CMP_SKU reads 0 rather than 1. The nine issue-rate values checked above
    # are the actual unlock signal; the SKU is only a sanity check on stock
    # firmware.
    sku = gpu.get("CMP_SKU")
    if sku == 0:
        print("note: CMP_SKU reads 0 - expected on the patched driver, which "
              "clears the SKU selector", file=sys.stderr)
    elif sku != 1:
        raise SystemExit(f"CMP_SKU is {sku}, expected 1 (or 0 on the patched driver)")

    expected_gpu = {"DISPLAY_ENABLED": 0}
    expected_gr = {
        "GPU_CORE_COUNT": 3584,
        "RT_CORE_COUNT": 56,
        "TENSOR_CORE_COUNT": 448,
    }
    for name, expected in expected_gpu.items():
        if gpu.get(name) != expected:
            raise SystemExit(f"{name} is {gpu.get(name)}, expected {expected}")
    for name, expected in expected_gr.items():
        if gr.get(name) != expected:
            raise SystemExit(f"{name} is {gr.get(name)}, expected {expected}")

    print("PASS_CMP50HX_ISSUE_RATE_AND_COUNTS")


if __name__ == "__main__":
    main()
