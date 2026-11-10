# Contributing to Nemyu

Nemyu is developed on GitLab at <https://gitlab.com/nemyu-emulator/nemyu>. Bug reports, testing, documentation and code are all welcome. This page explains how to report problems and how to send changes.

## Reporting Bugs and Requesting Features

Bugs and feature requests go to the [GitLab issue tracker](https://gitlab.com/nemyu-emulator/nemyu/-/issues).

- Search the existing issues first, and add to an existing one instead of opening a duplicate.
- Pick the **Bug** or **Feature** template when you open an issue, and fill in every section.
- For bugs, include:
  - the build you used (the release tag, or the commit shown in **Help > About Nemyu**);
  - what happened, and what you expected to happen;
  - steps to reproduce it, with a save file if it helps;
  - the log file (see [How to access logs](docs/user/HowToAccessLogs.md));
  - your CPU, GPU and driver version, RAM and operating system.
- Never attach or link keys, firmware or game files. Nemyu only supports content dumped from hardware you own.

## Code Contributions

Nemyu is free, open-source, copyleft software, licensed under the [GNU General Public License, version 3 or later](LICENSE.txt). Some parts are under other compatible licenses, whose texts are in [LICENSES](LICENSES). Before contributing, make sure you are familiar with the GPL, [its FAQ](https://www.gnu.org/licenses/gpl-faq.html) and [the concept of free software](https://www.gnu.org/philosophy/free-sw.html). If these terms are not acceptable to you, do not contribute.

### Policies

- **AI policy.** Read the [AI policy](docs/policies/AI.md) before using AI or LLM-based tools for a contribution. In short:
  - Disclose any AI use in both the commit message and the merge request description. An `Assisted-by:` trailer is the recommended form, for example `Assisted-by: <model> (<what it was used for>)`.
  - You must be able to explain, defend and revise every line you submit, and you must test it yourself.
  - Commit messages, merge request descriptions, issues and comments must be written by you, not pasted from an AI tool.
  - AI tools must not add `Signed-off-by:`, `Co-authored-by:` or `Co-developed-by:` trailers. The human submitter is the author of the change and is responsible for it.
- **Coding guidelines.** Read the [coding guidelines](docs/policies/Coding.md). New code must follow the general style of the surrounding code; exceptions may be granted in some cases.
- **Development guidelines.** Read the [development guidelines](docs/Development.md) for license headers, merge request naming and how to add settings.
- Keep comments and reviews focused on the change. Profanity rarely helps anyone understand what is going on, so avoid it.
- Maintainers may change your patches and merge requests when needed, although we try to avoid it.
  - In general, respect the decisions of the [code owners](docs/CODEOWNERS) of the area you are changing.
  - If you think they are wrong, explain why. Maintainers are not always right.
- Nemyu does not have a formal code of conduct, but you are expected to treat other developers, contributors and users with respect.
- You need a working knowledge of [Git](https://git-scm.com/learn), including managing branches in a fork and rebasing.

### Where to Start

Anywhere you like. If something is broken for you and you want to fix it, go ahead. You can also look through the [open issues](https://gitlab.com/nemyu-emulator/nemyu/-/issues).

For new features and large refactors, open a feature request first and describe the motivation, the planned implementation and how users will interact with it. If you want to work on an existing issue, comment on it first so that two people do not work on the same thing.

### Sending a Merge Request

1. Fork [the repository](https://gitlab.com/nemyu-emulator/nemyu) on GitLab and create a branch from `nemyu`, the default branch.
2. Build and test your change on the platforms it affects. See the [build documentation](docs/Build.md).
3. Add license headers to the files you create or change. See [License Headers](docs/Development.md#license-headers).
4. Write concise commit messages that explain the intent of the change, such as the bug it fixes or the feature it adds, rather than listing every line that changed. Disclose any AI use as described above.
5. Rebase your branch onto the latest `nemyu` branch.
6. Open a [merge request](https://gitlab.com/nemyu-emulator/nemyu/-/merge_requests) against `nemyu`, and complete the checklist in the merge request template.
7. Respond to review comments and update your branch until the merge request is ready.

## Other Contributions

- **Testing.** Trying games on your hardware and reporting bugs and regressions, with logs, helps a lot.
- **Documentation.** Developer documentation is in [docs](docs/README.md) and the user handbook is in [docs/user](docs/user/README.md). Send changes to either as a merge request.
- **Box art.** Box art for games without icons lives in the separate [boxart repository](https://gitlab.com/nemyu-emulator/boxart); missing or better covers can be contributed there.

If you have an external tool or page that you think Nemyu should link to or integrate, open a feature request. **Do not submit vibe-coded or AI-generated tools or applications.**
