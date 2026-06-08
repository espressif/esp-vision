Add a New Python Module
=======================

:link_to_translation:`zh_CN:[中文]`

ESP-VISION exposes Python modules through the MicroPython ``USER_C_MODULES``
mechanism. The binding layer lives in ``modules/`` and only does object
conversion plus light API adaptation; heavy logic belongs in pure C or in
``platform/``. This guide adds a module named ``foo``.

Overview
--------

.. code-block:: text

   modules/py_foo.c            # binding + MP_REGISTER_MODULE(MP_QSTR_foo, ...)
   modules/qstrdefs_esp_vision.h   # any feature-gated qstrs
   micropython.cmake           # add py_foo.c to the source list
   stubs/foo.pyi               # type stub for IDE completion
   docs/.../api-reference/foo.rst  # reference page

1. Create the Binding Source
----------------------------

Add ``modules/py_foo.c``. Define the module's functions, build a globals dict,
and self-register the module. Existing modules are the best templates; for a
simple function-only module follow ``modules/py_sensor.c``, and for a
type/class module follow ``modules/py_display.c``.

.. code-block:: c

   /*
    * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
    *
    * SPDX-License-Identifier: Apache-2.0
    */
   #include "py/runtime.h"

   static mp_obj_t foo_hello(void) {
       return mp_obj_new_int(42);
   }
   static MP_DEFINE_CONST_FUN_OBJ_0(foo_hello_obj, foo_hello);

   static const mp_rom_map_elem_t foo_module_globals_table[] = {
       { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_foo) },
       { MP_ROM_QSTR(MP_QSTR_hello), MP_ROM_PTR(&foo_hello_obj) },
   };
   static MP_DEFINE_CONST_DICT(foo_module_globals, foo_module_globals_table);

   const mp_obj_module_t mp_module_foo = {
       .base = { &mp_type_module },
       .globals = (mp_obj_dict_t *)&foo_module_globals,
   };

   MP_REGISTER_MODULE(MP_QSTR_foo, mp_module_foo);

C++ modules use ``.cpp`` (see ``modules/py_espdl.cpp``); the build already sets
``-std=gnu++2b`` for C++ sources.

2. Register qstrs If Needed
---------------------------

Most qstrs (``MP_QSTR_foo``, ``MP_QSTR_hello``) are collected automatically from
the source. Only add entries to ``modules/qstrdefs_esp_vision.h`` for names that
are board-feature-gated or otherwise not seen by the qstr scanner, using the
``Q(name)`` form.

3. Wire the Source into the Build
---------------------------------

Add the source to ``ESP_VISION_MODULE_SOURCES`` in ``micropython.cmake``:

.. code-block:: cmake

   set(ESP_VISION_MODULE_SOURCES
       ${ESP_VISION_ROOT}/modules/py_display.c
       ${ESP_VISION_ROOT}/modules/py_image.c
       ${ESP_VISION_ROOT}/modules/py_foo.c   # new
       ...
   )

If the module is only valid on some targets, add it inside the matching
``IDF_TARGET`` block (the H.264 and RTSP modules are gated to ``esp32p4`` this
way). If it needs an extra component, link it via ``target_link_libraries`` like
``idf::zxing``.

4. Add a Type Stub
------------------

Create ``stubs/foo.pyi`` describing the public surface so IDEs can complete it.
Keep the Apache SPDX header and match the style of the existing stubs.

5. Document the Module
----------------------

Add ``docs/en/api-reference/foo.rst`` and ``docs/zh_CN/api-reference/foo.rst``
using the Python-domain directives (``.. py:module::``, ``.. py:function::``,
``.. py:class::``), and add ``foo`` to the ``toctree`` in
``api-reference/index.rst`` for both languages.

6. Build and Verify
-------------------

.. code-block:: bash

   make BOARD=ESP32_P4X_EYE build

Then check from the REPL:

.. code-block:: python

   import foo
   print(foo.hello())
