#!/usr/bin/env python3
"""Validate a native/mounted named-operation TSV and optionally emit JSON."""
import argparse
import csv
import json
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('report',type=Path)
parser.add_argument('--json',dest='json_path',type=Path)
args=parser.parse_args()
rows=[];summary=None
for line in args.report.read_text().splitlines():
    fields=next(csv.reader([line],delimiter='\t'))
    if fields[:1]==['QFUSE_CASE']:
        if len(fields)!=9:raise SystemExit('Malformed named-case record')
        _,name,native,mounted,native_error,mounted_error,native_value,mounted_value,status=fields
        row={'name':name,'native_result':int(native),'mounted_result':int(mounted),
             'native_errno':int(native_error),'mounted_errno':int(mounted_error),
             'native_value':native_value,'mounted_value':mounted_value,'status':status}
        rows.append(row)
    elif fields[:1]==['QFUSE_CASE_SUMMARY']:
        if len(fields)!=3 or summary is not None:raise SystemExit('Malformed or duplicate summary')
        summary=(int(fields[1]),int(fields[2]))
if len(rows)<40 or len({row['name'] for row in rows})!=len(rows):raise SystemExit('Need at least forty unique named cases')
failed=[row['name'] for row in rows if row['status']!='PASS' or
        row['native_result']!=row['mounted_result'] or row['native_errno']!=row['mounted_errno'] or
        row['native_value']!=row['mounted_value']]
if failed:raise SystemExit('Differential mismatch: '+', '.join(failed))
if summary!=(len(rows),0):raise SystemExit('Missing or inconsistent successful summary')
if args.json_path:
    args.json_path.write_text(json.dumps({'case_count':len(rows),'error_cases':sum(row['native_result']<0 for row in rows),'cases':rows},indent=2)+'\n')
print(f"PASS named conformance report: {len(rows)} unique cases with identical semantic results, errno and recorded state")
