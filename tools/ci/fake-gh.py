#!/usr/bin/env python3
# A fake gh for tools/ci/test-release.py, installed as "gh" in its PATH. It
# keeps the releases in $FAKE_GH/releases.json and the workflow runs in
# $FAKE_GH/runs.json, logs every call to $FAKE_GH/log, and behaves like gh for
# the calls the release scripts make. $FAKE_CREATE sets what "release create"
# does: ok, partial (uploads one file, leaves a starter asset, fails) or fail.
import hashlib, json, os, subprocess, sys

d = os.environ["FAKE_GH"]
a = sys.argv[1:]

def load(name, default):
    p = os.path.join(d, name)
    return json.load(open(p)) if os.path.exists(p) else default

def save(name, obj):
    json.dump(obj, open(os.path.join(d, name), "w"))

def jq(obj):
    e = a[a.index("--jq") + 1]
    r = subprocess.run(["jq", "-r", e], input=json.dumps(obj), capture_output=True, text=True)
    if r.returncode:
        sys.exit("jq failed: " + r.stderr)
    sys.stdout.write(r.stdout)

def uploaded(f):
    return {"name": os.path.basename(f), "state": "uploaded",
            "digest": "sha256:" + hashlib.sha256(open(f, "rb").read()).hexdigest()}

def opt(name):
    for i, x in enumerate(a):
        if x == name:
            return a[i + 1]
        if x.startswith(name + "="):
            return x.split("=", 1)[1]
    return None

with open(os.path.join(d, "log"), "a") as log:
    log.write(" ".join(a[:3]) + "\n")

files = [x for x in a if x.startswith("out/") and os.path.isfile(x)]

if a[0] == "api":
    # Runs: [{"run_number", "status", "done_after"}]: a run's status becomes
    # "completed" once the API was called done_after times.
    calls = load("api-calls", 0) + 1
    save("api-calls", calls)
    if os.environ.get("FAKE_API") == "fail":
        sys.exit("HTTP 502")
    if "branch=superfw-next&event=push" not in a[1]:
        sys.exit("unexpected runs query: " + a[1])
    runs = load("runs.json", [])
    for r in runs:
        if calls >= r.get("done_after", 1 << 30):
            r["status"] = "completed"
    jq({"workflow_runs": runs})
    sys.exit(0)

rel = load("releases.json", {})
cmd, tag = a[1], a[2] if len(a) > 2 else None
if cmd == "list":
    jq([{"tagName": t} for t in rel])
elif cmd == "view":
    if tag not in rel:
        sys.exit("release not found")
    if "--jq" in a:
        jq(rel[tag])
elif cmd == "create":
    mode = os.environ.get("FAKE_CREATE", "ok")
    if mode == "fail":
        sys.exit("HTTP 502")
    assets = [uploaded(f) for f in files]
    if mode == "partial":
        assets = [assets[0], dict(assets[1], state="starter", digest=None)]
    rel[tag] = {"targetCommitish": opt("--target"), "latest": opt("--latest"),
                "notesStartTag": opt("--notes-start-tag"), "assets": assets}
    save("releases.json", rel)
    sys.exit(1 if mode == "partial" else 0)
elif cmd == "upload":
    have = {x["name"]: x for x in rel[tag]["assets"]}
    for f in files:
        if os.path.basename(f) in have and "--clobber" not in a:
            sys.exit("asset under the same filename already exists")
        have[os.path.basename(f)] = uploaded(f)
    rel[tag]["assets"] = list(have.values())
    save("releases.json", rel)
else:
    sys.exit("unexpected gh call: " + " ".join(a))
