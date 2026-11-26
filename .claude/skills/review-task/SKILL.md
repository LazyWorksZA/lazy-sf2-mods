---
name: review-task
description: Review a branch or task before merge - build a diff package, brief a fresh-context reviewer, pick the model tier, and run the fix loop with scoped re-reviews. Use after an implementer finishes a task or branch.
---

# Review a task

Reviews never merge or push. Merging into `master` needs the owner's explicit OK (AGENTS.md).

## 1. Build the review package

Run in the task's worktree. `BASE` is the branch point (usually `master`).

```powershell
$BASE = git merge-base master HEAD
$pkg = '<scratch>\review-<task>.diff'
(git log --oneline "$BASE..HEAD"; git diff --stat "$BASE..HEAD"; git diff -U10 "$BASE..HEAD") | Set-Content -Encoding utf8 $pkg
```

Check `git status` is clean and the build has no warnings. Keep the package in the scratch folder, never in the repo.

## 2. Brief the reviewer

The reviewer has fresh context, so give it everything and none of your conclusions.

```
Review the change in <package path> (worktree <path>, BASE..HEAD).
Spec: <the task text / brief section, verbatim>
Read AGENTS.md first; it is binding (no game files, RVA build checks, minimal comments, no warnings, CHANGELOG entry).
1. Spec check: each requirement, marked pass or fail, with evidence.
2. Findings, graded Critical (wrong behaviour, crash, rule break), Important (likely bug, missing test or changelog), Minor (style).
   Each finding: file:line and the concrete failure scenario. No finding without a scenario.
3. Reply in under 200 words. Write Minor findings to <scratch>\review-<task>-minors.md, not in the reply.
Do not edit code, commit or merge.
```

## 3. Model tiers

- First review of a task: the most capable model.
- Scoped re-review of a fix round (only the fix commits and the findings they answer): a mid-tier model.
- Pick the implementer's tier by the task, not by the review.

## 4. Fix loop

1. The implementer fixes Critical and Important findings in new commits (no history rewriting) and answers each finding: fixed (commit) or disagreed (reason).
2. Re-review only the fix range (`git diff -U10 <old HEAD>..HEAD`) against those findings. Do not reopen settled ground.
3. Record every ruling (accepted, rejected with reason, deferred Minor) in the task notes or commit messages so it is not argued twice.
4. Repeat until no Critical or Important remain. Then report to the owner: spec status, rulings, open Minors, live-test status (see `live-test`).
5. Stop reviewer and implementer agents once the work is merged or abandoned.
