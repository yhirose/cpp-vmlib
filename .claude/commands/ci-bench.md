---
description: Show the CI benchmark tables (executor + front ends) from a GitHub Actions run
argument-hint: "[run-id | branch]   default: the newest ci run on the current branch"
allowed-tools: Bash(gh:*), Bash(git rev-parse:*), Bash(git branch:*), Bash(cut:*), Bash(sed:*), Bash(grep:*)
---

Report the two tables `.github/workflows/ci.yml`'s `bench` job produces:
`## vmlib executor` (bench/bench.cc -- what a call, a binop or a variable
access costs the executor) and `## front ends vs. the real language`
(bench/languages/run.sh -- each front end next to the language it imitates).

Read them from the **step log**, not the run summary page. Both tables are
written with `tee -a "$GITHUB_STEP_SUMMARY"` precisely so they land in the
log too: GitHub exposes no API for reading a step summary back, so a
summary-only table is browser-only and unreachable from here.

`$ARGUMENTS` is an optional run id or branch name.

1. **Resolve the run.** A bare number is a run id -- use it directly.
   Anything else is a branch. Empty means the current branch
   (`git rev-parse --abbrev-ref HEAD`). For a branch:

   ```
   gh run list --workflow=ci.yml --branch <branch> --limit 1 \
     --json databaseId,status,conclusion,displayTitle,headSha,createdAt
   ```

2. **Find the bench job.**

   ```
   gh run view <run-id> --json jobs \
     --jq '.jobs[] | select(.name=="bench") | "\(.databaseId)\t\(.status)\t\(.conclusion)"'
   ```

3. **If it has not completed**, stop and say so, with the job's status and
   the run's own URL (`gh run view <run-id> --web`) -- a job's log does not
   exist until it finishes, and the summary page is the only way to see
   anything before then. Offer `gh run watch <run-id>`; do not poll on your
   own unless the user asks for it.

4. **Once it has completed**, pull the tables:

   ```
   gh run view --job=<job-id> --log \
     | cut -f3- \
     | sed -E 's/^[^ ]+ //' \
     | grep -E '^(##|\||warning:)'
   ```

   Each log line is `<job><TAB><step><TAB><ISO timestamp> <content>`, so
   `cut -f3-` (whose delimiter is already a tab) drops the first two fields
   and the `sed` drops the timestamp, leaving the markdown behind. Doing the
   tab work in `cut` rather than in a `[^\t]` bracket keeps this off GNU
   sed's own regex extensions. The `warning:`
   arm catches run.sh's own answer check -- it prints one to stderr when a
   front end's output does not match the workload's known result, which
   matters far more than any timing on the same row.

5. **Render both tables** to the user as markdown, then say in a sentence
   or two what actually stands out. Worth calling out, in this order:

   - any `warning:` line (a wrong answer beats a fast one);
   - a `-` in `real_ms` where that toolchain was supposed to be installed
     (the job installs lua5.4, guile-3.0, dotnet and culebra, and relies on
     the runner image for python3, ruby, node and go) -- a `-` there means
     an install step regressed, not that the language is slow;
   - a front end far off its own previous shape, or far off the real
     language on one workload but not the others.

   Read a workload's `real_ms` next to `startup`'s own row before calling a
   language slow: process startup is a tax every other row carries, and it
   ranges from about a millisecond (a compiled Go binary, lua5.4) to about
   fifty (python3). See bench/languages/run.sh's own header comment.

If the log has no `##` lines at all, the run predates the `tee` change --
say so and hand over `gh run view <run-id> --web`.
