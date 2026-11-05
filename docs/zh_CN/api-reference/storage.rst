storage -- 存储诊断与确认恢复
=============================

:link_to_translation:`en:[English]`

可在不修改数据的情况下检查卷、重试挂载，或明确确认恢复未挂载的 Flash ``ffat``。存储失败通过诊断结果返回；无效参数会抛出 Python 异常。数据保护、备份和恢复的完整规范见 :doc:`../storage/index`。恢复成功后，可能需要用户选择软复位来发布 Python 挂载。

当前不支持在线格式化 SD，也不支持格式化已挂载卷。用户同意擦除选定卷后，调用方才可传入布尔值 ``True``；不能在遇到错误后自动设置确认参数。

.. include:: _generated/storage.rst
