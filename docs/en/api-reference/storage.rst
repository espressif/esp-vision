storage -- Diagnostics and Confirmed Recovery
=============================================

:link_to_translation:`zh_CN:[中文]`

Inspect a volume without modifying it, retry mounting without formatting, or explicitly confirm recovery of an unmounted Flash ``ffat``. Storage failures return diagnostic results; invalid arguments raise Python exceptions. See :doc:`../storage/index` for the complete data-protection, backup and recovery contract. A successful recovery may require a user-selected soft reset to publish the Python mount.

Online SD formatting and formatting a mounted volume are not supported. Confirmation must be the literal boolean ``True`` after the user has approved erasing the selected volume; callers must not set it automatically after an error.

.. include:: _generated/storage.rst
