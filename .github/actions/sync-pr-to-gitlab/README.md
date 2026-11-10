# GitHub PR to GitLab MR sync

This action adapts [espressif/sync-pr-to-gitlab](https://github.com/espressif/sync-pr-to-gitlab) at commit `6ea5db60f6cc51c7485f6292f29e51ef92656e3f` (Apache-2.0). The Python source retains its original notice. The local adaptation removes Jira-specific title parsing and issue links, and treats an empty PR description as empty text. Other synchronization behavior follows that upstream version.
