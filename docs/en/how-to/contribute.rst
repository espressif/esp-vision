Contribution and Review
=======================

:link_to_translation:`zh_CN:[中文]`

Open a pull request (PR) against the appropriate branch in `ESP-VISION on GitHub <https://github.com/espressif/esp-vision>`__. Describe the change, its tests, and any hardware used. Follow the repository's copyright and license rules, and add an entry under ``Unreleased`` in ``CHANGELOG.md``. Maintainers review the PR on GitHub and may request changes there.

Sync and merge process
----------------------

.. only:: html

   .. mermaid::

      flowchart TD
          A[Contributor opens a GitHub PR] --> B[Maintainer reviews the PR]
          B --> C[Maintainer comments reviewed SHA and applies sync label]
          C --> D[Action creates or updates a GitLab MR]
          D --> E[GitLab review, CI, and hardware checks as needed]
          E -->|Changes needed| F[Contributor updates the PR]
          F --> C
          E -->|Approved| G[Merge GitLab MR]
          G --> H[GitLab CI syncs the branch to GitHub]
          H --> I[Maintainer verifies the GitHub PR status]

#. After review, the maintainer comments ``sha=<commit>`` with the current PR head SHA and applies ``PR-Sync-Merge`` or ``PR-Sync-Rebase``. The comment and label must come from the same maintainer. The current action rejects PRs that modify paths containing ``.github`` or ``.gitlab``; coordinate those changes with maintainers.
#. The action creates a GitLab MR. If the contributor updates the PR, the maintainer comments the new head SHA and applies or reapplies ``PR-Sync-Update`` while retaining the original merge or rebase label.
#. Maintainers review the GitLab MR and run the applicable CI and hardware checks. They communicate requested changes on the GitHub PR. Once approved, they merge the GitLab MR into its target branch.
#. GitLab CI publishes the tested branch to GitHub. Maintainers verify that the PR shows the correct status and link the resulting commit; if rewritten history prevents automatic recognition, they close the PR with an explanation.

The workflow depends on GitHub secrets and network access to the internal GitLab. A failed or pending sync does not mean that the contribution has been merged. No Jira issue or Jira project is required.
