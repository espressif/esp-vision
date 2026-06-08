添加新的 Python 模块
====================

:link_to_translation:`en:[English]`

ESP-VISION 通过 MicroPython 的 ``USER_C_MODULES`` 机制暴露 Python 模块。绑定层位于
``modules/``，仅做对象转换与轻量 API 适配；重逻辑应放在纯 C 或 ``platform/`` 中。本指南
以添加名为 ``foo`` 的模块为例。

总览
----

.. code-block:: text

   modules/py_foo.c            # 绑定 + MP_REGISTER_MODULE(MP_QSTR_foo, ...)
   modules/qstrdefs_esp_vision.h   # 任何受功能开关控制的 qstr
   micropython.cmake           # 将 py_foo.c 加入源文件列表
   stubs/foo.pyi               # 供 IDE 补全的类型存根
   docs/.../api-reference/foo.rst  # 参考页面

1. 创建绑定源文件
-----------------

新增 ``modules/py_foo.c``：定义模块函数，构建 globals 字典，并自注册模块。现有模块是最佳
模板——纯函数模块可参考 ``modules/py_sensor.c``，类型/类模块可参考
``modules/py_display.c``。

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

C++ 模块使用 ``.cpp``（参见 ``modules/py_espdl.cpp``）；构建已为 C++ 源文件设置
``-std=gnu++2b``。

2. 按需注册 qstr
----------------

大多数 qstr（``MP_QSTR_foo``、``MP_QSTR_hello``）会自动从源码收集。仅当名称受板级功能开关
控制、或无法被 qstr 扫描器识别时，才需以 ``Q(name)`` 形式添加到
``modules/qstrdefs_esp_vision.h``。

3. 将源文件接入构建
-------------------

在 ``micropython.cmake`` 的 ``ESP_VISION_MODULE_SOURCES`` 中加入该源文件：

.. code-block:: cmake

   set(ESP_VISION_MODULE_SOURCES
       ${ESP_VISION_ROOT}/modules/py_display.c
       ${ESP_VISION_ROOT}/modules/py_image.c
       ${ESP_VISION_ROOT}/modules/py_foo.c   # 新增
       ...
   )

若模块仅在部分目标上有效，请放入对应的 ``IDF_TARGET`` 判断块中（H.264 与 RTSP 模块即以此
方式限定为 ``esp32p4``）。若需额外组件，可像 ``idf::zxing`` 那样用
``target_link_libraries`` 链接。

4. 添加类型存根
---------------

创建 ``stubs/foo.pyi`` 描述公开接口，便于 IDE 补全。保留 Apache SPDX 头并与现有存根风格
保持一致。

5. 编写模块文档
---------------

新增 ``docs/en/api-reference/foo.rst`` 与 ``docs/zh_CN/api-reference/foo.rst``，使用
Python 域指令（``.. py:module::``、``.. py:function::``、``.. py:class::``），并在两种
语言的 ``api-reference/index.rst`` 的 ``toctree`` 中加入 ``foo``。

6. 构建并验证
-------------

.. code-block:: bash

   make BOARD=ESP32_P4X_EYE build

然后在 REPL 中检查：

.. code-block:: python

   import foo
   print(foo.hello())
