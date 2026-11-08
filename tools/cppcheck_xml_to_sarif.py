#!/usr/bin/env python3
"""cppcheck_xml_to_sarif.py -- convert cppcheck --xml (version 2) to SARIF.

cppcheck gained --output-format=sarif after 2.13 (Ubuntu 24.04 ships 2.13.0,
which rejects the flag and checks nothing). scripts/analyzer_lanes_probe.sh
uses this converter when the installed cppcheck has no native SARIF output,
so the lane's downstream consumers read one shape on every version.

    cppcheck_xml_to_sarif.py IN.xml OUT.sarif
    cppcheck_xml_to_sarif.py --self-test

A missing or unparseable input is an error (exit 2), never an empty log.
"""
import json
import os
import sys
import tempfile
import xml.etree.ElementTree as ET

LEVELS = {"error": "error", "warning": "warning"}


def convert(xml_path):
    root = ET.parse(xml_path).getroot()
    if root.tag != "results":
        raise ValueError("not a cppcheck --xml file: root is <%s>" % root.tag)
    ver = root.find("cppcheck")
    version = ver.get("version", "unknown") if ver is not None else "unknown"
    results = []
    for err in root.iter("error"):
        locs = []
        for loc in err.findall("location"):
            f = loc.get("file")
            if not f:
                continue
            locs.append({"physicalLocation": {
                "artifactLocation": {"uri": os.path.abspath(f)},
                "region": {"startLine": max(int(loc.get("line", "1")), 1)}}})
        res = {"ruleId": err.get("id", ""),
               "level": LEVELS.get(err.get("severity", ""), "note"),
               "message": {"text": err.get("msg", "")}}
        if not locs and err.get("file0"):
            locs.append({"physicalLocation": {
                "artifactLocation": {"uri": os.path.abspath(err.get("file0"))},
                "region": {"startLine": 1}}})
        if locs:
            # cppcheck lists the primary location last
            res["locations"] = [locs[-1]]
        results.append(res)
    return {"version": "2.1.0",
            "$schema": "https://json.schemastore.org/sarif-2.1.0.json",
            "runs": [{"tool": {"driver": {"name": "Cppcheck", "version": version}},
                      "results": results}]}


def self_test():
    xml = ('<?xml version="1.0"?><results version="2"><cppcheck version="2.13.0"/>'
           '<errors><error id="nullPointer" severity="error" msg="Null pointer">'
           '<location file="/x/bug.c" line="7" column="3"/></error>'
           '<error id="misra-c2012-8.4" severity="style" msg="m">'
           '<location file="/x/bug.c" line="2" column="1"/></error></errors></results>')
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "a.xml")
        open(p, "w").write(xml)
        doc = convert(p)
        r = doc["runs"][0]["results"]
        ok = (len(r) == 2 and r[0]["ruleId"] == "nullPointer" and r[0]["level"] == "error"
              and r[1]["level"] == "note"
              and r[0]["locations"][0]["physicalLocation"]["region"]["startLine"] == 7
              and doc["runs"][0]["tool"]["driver"]["version"] == "2.13.0")
        open(os.path.join(d, "bad.xml"), "w").write("<results")
        try:
            convert(os.path.join(d, "bad.xml"))
            ok = False  # a broken input must not convert
        except ET.ParseError:
            pass
    print("cppcheck_xml_to_sarif self-test: " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    try:
        doc = convert(argv[1])
    except (OSError, ET.ParseError, ValueError) as e:
        sys.stderr.write("cppcheck_xml_to_sarif: %s\n" % e)
        return 2
    with open(argv[2], "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
