#!/usr/bin/env python3
# Tests tools/ci/publish-release.sh and tools/ci/wait-earlier-runs.sh against
# a fake gh (tools/ci/fake-gh.py), so the release paths of
# .github/workflows/superfw-next.yml run in every pull request.
import hashlib, json, os, shutil, subprocess, sys, tempfile, types

CI = os.path.dirname(os.path.abspath(__file__))
FILES = ["superfw-sd.fw", "superfw-lite.fw", "superfw-chis.fw"]   # As upstream's releases
failures = []

def build(v, sha):
    # The files a build of commit sha makes: {name: content}
    return {f: ("%s %s %s\n" % (v, sha, f)).encode() for f in FILES}

def release(v, sha, starter=None, author="github-actions[bot]"):
    assets = [{"name": n, "state": "uploaded", "digest": "sha256:" + hashlib.sha256(c).hexdigest()}
              for n, c in build(v, sha).items()]
    if starter:
        assets = [dict(x, state="starter", digest=None) if x["name"].endswith(starter) else x
                  for x in assets]
    return {"author": {"login": author}, "targetCommitish": sha, "assets": assets}

def run(script, env, releases=None, runs=None, out=None):
    d = tempfile.mkdtemp()
    os.makedirs(d + "/bin")
    os.symlink(CI + "/fake-gh.py", d + "/bin/gh")
    os.makedirs(d + "/out")
    for n, c in (out or {}).items():
        open(d + "/out/" + n, "wb").write(c)
    json.dump(releases or {}, open(d + "/releases.json", "w"))
    json.dump(runs or [], open(d + "/runs.json", "w"))
    e = dict(os.environ, PATH=d + "/bin:" + os.environ["PATH"], FAKE_GH=d, WAIT_SECONDS="0", **env)
    try:
        p = subprocess.run(["bash", CI + "/" + script], cwd=d, env=e, capture_output=True, text=True, timeout=20)
    except subprocess.TimeoutExpired:
        p = types.SimpleNamespace(returncode=124, stdout="", stderr="timed out")
    calls = open(d + "/log").read().splitlines() if os.path.exists(d + "/log") else []
    rel = json.load(open(d + "/releases.json"))
    api = json.load(open(d + "/api-calls")) if os.path.exists(d + "/api-calls") else 0
    shutil.rmtree(d)
    return p, rel, calls, api

def check(name, cond, p):
    print("%s: %s" % ("ok  " if cond else "FAIL", name))
    if not cond:
        failures.append(name)
        print("  " + (p.stdout + p.stderr).strip().replace("\n", "\n  "))

def complete(rel, v, sha):
    # The release has every file, uploaded, with the content of sha's build
    want = {n: "sha256:" + hashlib.sha256(c).hexdigest() for n, c in build(v, sha).items()}
    got = {x["name"]: x["digest"] for x in rel.get("next-" + v, {}).get("assets", []) if x["state"] == "uploaded"}
    return got == want

def publish(name, v, sha, releases, expect_ok, create="ok", out=None, test=None, list_="ok"):
    p, rel, calls, _ = run("publish-release.sh", {"V": v, "GITHUB_SHA": sha, "FAKE_CREATE": create, "FAKE_LIST": list_},
                           releases=releases, out=build(v, sha) if out is None else out)
    check(name, (p.returncode == 0) == expect_ok and (test is None or test(rel, calls)), p)

V1, V2, V3 = "v0.1", "v0.2", "v0.3"
writes = lambda calls: [c for c in calls if c.startswith(("release create", "release upload"))]

publish("first release", V2, "aaa1", {}, True,
        test=lambda r, c: complete(r, V2, "aaa1") and r["next-v0.2"]["latest"] == "true"
        and r["next-v0.2"]["notesStartTag"] is None and r["next-v0.2"]["targetCommitish"] == "aaa1")
publish("new version: notes since the previous one, latest", V2, "aaa1", {"next-v0.1": release(V1, "old1")}, True,
        test=lambda r, c: complete(r, V2, "aaa1") and r["next-v0.2"]["latest"] == "true"
        and r["next-v0.2"]["notesStartTag"] == "next-v0.1")
publish("older version published after a newer one: not latest", V2, "aaa1",
        {"next-v0.1": release(V1, "old1"), "next-v0.3": release(V3, "ccc3")}, True,
        test=lambda r, c: complete(r, V2, "aaa1") and r["next-v0.2"]["latest"] == "false"
        and r["next-v0.2"]["notesStartTag"] == "next-v0.1")
publish("create leaves a starter asset: uploaded again", V2, "aaa1", {"next-v0.1": release(V1, "old1")}, True,
        create="partial", test=lambda r, c: complete(r, V2, "aaa1"))
publish("listing the releases fails: error, nothing published", V2, "aaa1",
        {"next-v0.1": release(V1, "old1"), "next-v0.3": release(V3, "ccc3")}, False, list_="fail",
        test=lambda r, c: not writes(c))
publish("create fails, nothing published: error", V2, "aaa1", {}, False, create="fail",
        test=lambda r, c: "next-v0.2" not in r)
publish("already released by this commit (rerun): nothing to do", V2, "aaa1", {"next-v0.2": release(V2, "aaa1")}, True,
        test=lambda r, c: not writes(c) and complete(r, V2, "aaa1"))
publish("already released by another commit (same version): nothing to do", V2, "bbb2",
        {"next-v0.2": release(V2, "aaa1")}, True, test=lambda r, c: not writes(c) and complete(r, V2, "aaa1"))
publish("this commit's release with a starter asset: repaired", V2, "aaa1",
        {"next-v0.2": release(V2, "aaa1", starter="sd.fw")}, True, test=lambda r, c: complete(r, V2, "aaa1"))
publish("this commit's release with a wrong file: replaced", V2, "aaa1",
        {"next-v0.2": dict(release(V2, "zzz9"), targetCommitish="aaa1")}, True, test=lambda r, c: complete(r, V2, "aaa1"))
publish("another commit's release with a starter asset: error, untouched", V2, "bbb2",
        {"next-v0.2": release(V2, "aaa1", starter="sd.fw")}, False, test=lambda r, c: not writes(c))
# ie. next-v0.1, published before CI with only the SD card's firmware
hand = release(V2, "aaa1", author="vinivius")
hand["assets"] = [x for x in hand["assets"] if x["name"] == "superfw-sd.fw"]
publish("a release made by hand: left as it is", V2, "bbb2", {"next-v0.2": hand}, True,
        test=lambda r, c: not writes(c))
publish("a build file is missing: error", V2, "aaa1", {}, False,
        out={n: c for n, c in build(V2, "aaa1").items() if n != "superfw-lite.fw"},
        test=lambda r, c: not writes(c))

def wait(name, runs, expect_ok, calls, api="ok"):
    p, _, _, n = run("wait-earlier-runs.sh", {"GITHUB_REPOSITORY": "o/r", "GITHUB_RUN_NUMBER": "10", "FAKE_API": api},
                     runs=runs)
    check(name + " (%d API calls)" % n, (p.returncode == 0) == expect_ok and n == calls, p)

wait("no earlier run in progress (later and finished ones don't count)",
     [{"run_number": 9, "status": "completed"}, {"run_number": 11, "status": "in_progress"},
      {"run_number": 10, "status": "in_progress"}], True, 1)
wait("waits for the earlier runs to finish",
     [{"run_number": 8, "status": "queued", "done_after": 2}, {"run_number": 9, "status": "in_progress", "done_after": 3}],
     True, 3)
wait("the API keeps failing: error", [], False, 5, api="fail")

print("%d failed" % len(failures) if failures else "All release tests passed")
sys.exit(1 if failures else 0)
