#!/bin/bash
# wait-earlier-runs.sh: waits until the earlier push runs of the SuperFW Next
# workflow on superfw-next (lower run number) have finished, so releases are
# published in merge order. Needs GITHUB_REPOSITORY, GITHUB_RUN_NUMBER and gh
# with read access to Actions. Tested by tools/ci/test-release.py.
set -e

runs="repos/$GITHUB_REPOSITORY/actions/workflows/superfw-next.yml/runs?branch=superfw-next&event=push&per_page=100"
fails=0
while :; do
  if n=$(gh api "$runs" --jq "[.workflow_runs[] | select(.status != \"completed\" and .run_number < $GITHUB_RUN_NUMBER)] | length"); then
    [ "$n" = 0 ] && break
    echo "Waiting for $n earlier run(s) to finish..."
  else
    fails=$((fails + 1))
    [ $fails -lt 5 ] || { echo "::error::Could not list the workflow runs"; exit 1; }
    echo "Could not list the workflow runs, retrying..."
  fi
  sleep "${WAIT_SECONDS:-20}"
done
