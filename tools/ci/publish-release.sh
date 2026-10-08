#!/bin/bash
# publish-release.sh: publishes the firmware in out/ as the release next-$V,
# once per version. Run by .github/workflows/superfw-next.yml under its
# release lock, after a merge into superfw-next. Needs V (ie. v0.2),
# GITHUB_SHA and gh with write access. Tested by tools/ci/test-release.py.
set -e

tag="next-$V"
files=("out/superfw-next-$V-sd.fw" "out/superfw-next-$V-sd.gba" "out/superfw-next-$V-sd-uart-debug.gba" out/SHA256SUMS)
for f in "${files[@]}"; do
  [ -f "$f" ] || { echo "::error::$f is missing"; exit 1; }
done

if ! gh release view "$tag" >/dev/null 2>&1; then
  cat > notes.md <<NOTES
SuperFW Next ${V#v} for the Supercard SD, built from ${GITHUB_SHA::8}.

**Try it first:** copy \`superfw-next-$V-sd.gba\` to the SD card and launch it from the browser like a game. It runs from memory; turning the console off brings back your current firmware.

**Flash it:** with fresh batteries or a power adapter, copy \`superfw-next-$V-sd.fw\` to the SD card (keep only one \`.fw\` there). On the About tab press Down + B + START, pick the \`.fw\` in the browser, press A, then L + R + Up, and wait for "Flash update complete!".

\`superfw-next-$V-sd-uart-debug.gba\` is the serial debug build (see tools/debug). Checksums are in SHA256SUMS.
NOTES
  # An earlier version can still be published after a later one (ie.
  # rerunning an old run): notes start at the highest existing version below
  # this one, and only the highest one is the latest. The release lock keeps
  # this list current until it's published.
  # Listed first, so set -e stops on an API failure (an error must not look
  # like an empty release history); grep finding nothing is fine.
  list=$(gh release list --limit 200 --json tagName --jq '.[].tagName')
  tags=$( { grep '^next-v' <<< "$list" || true; echo "$tag"; } | sort -V -u)
  prev=$(echo "$tags" | grep -x -B1 "$tag" | grep -v -x "$tag" || true)
  latest=$([ "$(echo "$tags" | tail -n1)" = "$tag" ] && echo true || echo false)
  echo "Publishing $tag (notes since: ${prev:-the beginning}, latest: $latest)"
  gh release create "$tag" --target "$GITHUB_SHA" --title "SuperFW Next ${V#v}" \
    --notes "$(cat notes.md)" --generate-notes ${prev:+--notes-start-tag "$prev"} \
    --latest="$latest" "${files[@]}" || echo "Creating $tag failed, checking what was published"
fi

# Published (by this run, an earlier attempt or another run): every file must
# be fully uploaded (GitHub can leave an empty "starter" asset after a failed
# upload), and match this build byte for byte when the release was made from
# this commit. Broken or missing files are uploaded again, but never from
# another commit's build.
gh release view "$tag" >/dev/null 2>&1 || { echo "::error::$tag could not be published"; exit 1; }
# Releases made by hand (ie. next-v0.1, from before CI) have other files.
author=$(gh release view "$tag" --json author --jq .author.login)
if [ "$author" != "github-actions[bot]" ]; then
  echo "$tag was published by hand ($author): left as it is."
  exit 0
fi
target=$(gh release view "$tag" --json targetCommitish --jq .targetCommitish)
check() {
  local assets f n a
  assets=$(gh release view "$tag" --json assets --jq '.assets[] | "\(.name) \(.state) \(.digest // "")"')
  redo=()
  for f in "${files[@]}"; do
    n=$(basename "$f")
    a=$(awk -v n="$n" '$1 == n' <<< "$assets")
    if [ "$target" = "$GITHUB_SHA" ]; then
      [ "$a" = "$n uploaded sha256:$(sha256sum "$f" | cut -d' ' -f1)" ] || redo+=("$f")
    else
      [ "$(cut -d' ' -f2 <<< "$a")" = uploaded ] || redo+=("$f")
    fi
  done
}
check
if [ ${#redo[@]} -gt 0 ]; then
  if [ "$target" != "$GITHUB_SHA" ]; then
    echo "::error::$tag (from $target) is missing ${redo[*]##*/}: fix it by hand, or bump VERSION_WORD."
    exit 1
  fi
  echo "Uploading again: ${redo[*]##*/}"
  gh release upload "$tag" --clobber "${redo[@]}"
  check
  if [ ${#redo[@]} -gt 0 ]; then
    echo "::error::$tag is still missing ${redo[*]##*/}"
    exit 1
  fi
fi
echo "$tag has all its files."
