贡献与审阅
==========

:link_to_translation:`en:[English]`

请在 `GitHub 上的 ESP-VISION <https://github.com/espressif/esp-vision>`__ 向对应分支提交 Pull Request（PR），说明改动、测试和使用的硬件。遵循仓库的版权与许可证规则，并在 ``CHANGELOG.md`` 的 ``Unreleased`` 部分添加条目。维护者在 GitHub 上审阅 PR，并在需要时提出修改意见。

同步与合并流程
--------------

.. only:: html

   .. mermaid::

      flowchart TD
          A[贡献者提交 GitHub PR] --> B[维护者审阅 PR]
          B --> C[维护者评论已审阅的 SHA 并添加同步标签]
          C --> D[Action 创建或更新 GitLab MR]
          D --> E[GitLab 审阅、CI 和必要的硬件测试]
          E -->|需要修改| F[贡献者更新 PR]
          F --> C
          E -->|通过| G[合并 GitLab MR]
          G --> H[GitLab CI 将分支同步到 GitHub]
          H --> I[维护者确认 GitHub PR 状态]

#. 审阅完成后，维护者以 ``sha=<commit>`` 评论当前 PR 的最新提交 SHA，并添加 ``PR-Sync-Merge`` 或 ``PR-Sync-Rebase`` 标签。评论和标签必须由同一位维护者提交。当前 Action 会拒绝修改路径中包含 ``.github`` 或 ``.gitlab`` 的 PR；此类修改请与维护者协调。
#. Action 创建 GitLab MR。贡献者更新 PR 后，维护者评论新的最新提交 SHA，并在保留原合并或变基标签的同时添加或重新添加 ``PR-Sync-Update``。
#. 维护者在 GitLab 审阅 MR，运行适用的 CI 和硬件测试，并在 GitHub PR 上反馈需要修改的内容。通过后，将 MR 合并到 GitLab 的目标分支。
#. GitLab CI 将通过测试的分支同步到 GitHub。维护者核对 PR 状态并关联最终提交；如果改写提交历史导致 GitHub 未自动识别，则说明情况后手动关闭 PR。

工作流需要配置 GitHub 密钥并能够访问内部 GitLab。同步失败或尚未完成不代表贡献已经合并。此流程不需要 Jira 项目或 Jira issue。
