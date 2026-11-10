# Testing

When you're testing a merge request (MR) and encounter unexpected behavior, it's important to determine whether the issue was introduced by the MR or if it already exists in the base code. To do this, compare the behavior against the branch the MR is based on (usually `nemyu`).

Even before an issue occurs, it is best practice to keep the same settings and delete the shader cache. Using an already made shader cache can make the MR look like it is having a regression in some rare cases.

Try not to test MRs which are for documentation or extremely trivial changes (like an MR that changes the app icon), unless you really want to.

If an MR specifies it is for a given platform (i.e `linux`) then just test on Linux. If it says `NCE` then test on Linux ARM64 (Raspberry Pi and such). macOS fixes may also affect Asahi, test that if you can too.

The repository has no GitLab CI pipeline, so merge requests don't come with prebuilt binaries: build the MR's branch yourself by following the [build guide](../Build.md). Be aware that builds made on different machines are NOT the same, because of package versioning and build environment differences. One famous example is FFmpeg randomly breaking on many Arch distros due to packaging differences.

## Quickstart

Think of the source code as a "tree", with the "trunk" of that tree being our `nemyu` branch, any other branches are MRs or separate development branches, only our releases are made from `nemyu` - all other branches are considered unstable and aren't recommended to pull from unless you're testing multiple branches at once.

Here's some terminology you may want to familiarize yourself with:

- MR: Merge request, a change in the codebase; from which the author of said change (the programmer) requests a merge of that branch into `nemyu` (make it so the new code makes it into a release basically). GitHub and Forgejo call this a pull request (PR).
- Bisect: Binary method of searching regressions, some regressions may be sporadic and can't be bisected, but the overwhelming majority are.
- WIP: Work-in-progress.
- Regression: A new bug/glitch caused by new code, i.e "Zelda broke on Linux after commit xyz".
- `nemyu`: The default branch, this is where all merged code goes to (other projects call it `main`, `trunk` or `master`), it contains all the code that eventually makes it to releases.
- `HEAD`: Latest commit in a given branch, `HEAD` of `nemyu` is the latest commit on branch `nemyu`.
- `origin`: The default "remote", basically the URL from where git is located at, for most of the time that location is https://gitlab.com/nemyu-emulator/nemyu.

## Testing checklist

For regressions/bugs from MRs or commits:

- [ ] Occurs on `nemyu`?
    - If it occurs on `nemyu`:
        - [ ] Occurs on previous release? (before this particular MR).
            - If it occurs on previous release:
                - [ ] Occurs on previous-previous release?
                    - And so on and so forth... some bugs come from way before Nemyu (or even Eden) was conceived.
            - Otherwise, try bisecting between the previous release AND the latest `HEAD` of `nemyu`
                - [ ] Occurs in given commit?
- [ ] Occurs in MR?
    - If it occurs on MR:
        - [ ] Bisected MR? (if it has commits)
        - [ ] Found bisected commit?

If an issue sporadically appears, try to do multiple runs, try if possible, to count the number of times it has failed and the number of times it has "worked just fine"; say it worked 3 times but failed 1. then there is a 1/4th chance every run that the issue is replicated - so every bisect step would require 4 runs to ensure there is at least a chance of triggering the bug.

## What to do when something seems off

If you notice something odd during testing:

- Reproduce the issue using the base branch.
- Observe whether the same behavior occurs.

From there onwards there can be two possible outcomes:

- If the issue exists in the base branch: This means the problem was already present before the MR. The MR most likely did not introduce the regression.
- If the issue does not exist in the base branch: This suggests the MR most likely introduced the regression and needs further investigation.

## Reporting Your Findings

When you report your results:

- Clearly state whether the behavior was observed in the base branch.
- Indicate whether the result is good (expected behavior) or bad (unexpected or broken behavior). Without mentioning if your post/report/log is good or bad it may confuse the developer of the MR.

For example:

1. "Bad result for MR: Tested on base branch - issue not present. Likely regression introduced."
2. "Good result for MR: Tested on base branch - issue already present. Not a regression."

This approach helps maintain clarity and accountability in the testing process and ensures regressions are caught and addressed efficiently. If the behavior seems normal for a certain game/feature then it may not be always required to check against the base branch.

If there is no build of the MR's base commit, it will be helpful to just test past and future builds nearby. That would help with gathering more information about the problem.

**Always include [debugging info](../Debug.md) as needed**.

## Bisecting

One happy reminder, when testing, *know how to bisect!*

Say you're trying to find an issue between 1st of Jan and 8th of Jan, you can search by dividing "in half" the time between each commit:
- Check for 4th of Jan
- If 4th of Jan is "working" then the issue must be in the future
- So then check 6th of Jan
- If 6th of Jan isn't working then the issue must be in the past
- So then check 5th of Jan
- If 5th of Jan worked, then the issue starts at 6th of Jan

The faulty commit then, is 6th of Jan. This is called bisection https://git-scm.com/docs/git-bisect

## Notes

- MRs marked as drafts (**Draft:** or **WIP**) do NOT need to be tested unless explicitly asked.
- All open merge requests can be viewed [here](https://gitlab.com/nemyu-emulator/nemyu/-/merge_requests).
