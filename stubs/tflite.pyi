# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from typing import Callable, Sequence

#: Tensor shape tuple, for example (1, 96, 96, 1).
Shape = tuple[int, ...]
#: Model input item. Use an ulab ndarray for normal numeric input, or a callable to fill the raw tensor buffer.
TensorInput = object
#: Model output item. Outputs are returned as ulab ndarrays with raw model dtype and quantized values.
TensorOutput = object
#: Callable input writer: (buffer, shape, dtype_code).
TensorWriter = Callable[[bytearray, Shape, int], None]
#: Optional post-processing callback: (model, inputs, raw_outputs).
Postprocess = Callable[["Model", Sequence[TensorInput | TensorWriter], list[TensorOutput]], object]


#: TensorFlow Lite Micro model wrapper.
class Model:
    #: Model file length in bytes.
    len: int
    #: Runtime tensor arena size in bytes.
    ram: int
    #: Input tensor shapes.
    input_shape: tuple[Shape, ...]
    #: Input quantization scales.
    input_scale: tuple[float, ...]
    #: Input quantization zero points.
    input_zero_point: tuple[int, ...]
    #: Input dtype codes as strings, such as "b", "B", "h", "H", or "f".
    input_dtype: tuple[str, ...]
    #: Output tensor shapes.
    output_shape: tuple[Shape, ...]
    #: Output quantization scales.
    output_scale: tuple[float, ...]
    #: Output quantization zero points.
    output_zero_point: tuple[int, ...]
    #: Output dtype codes as strings, such as "b", "B", "h", "H", or "f".
    output_dtype: tuple[str, ...]
    #: Default post-processing callback used by predict() when callback is not passed.
    postprocess: Postprocess | None

    #: Load a .tflite flatbuffer from the filesystem.
    #: path: model path, for example "/sdcard/model.tflite" or "/flash/model.tflite".
    #: postprocess: optional callback that receives (model, inputs, raw_outputs) and returns the final result.
    def __init__(self, path: str, *, postprocess: Postprocess | None = None) -> None: ...
    def __del__(self) -> None: ...
    #: Release model, interpreter, and tensor arena resources.
    def deinit(self) -> None: ...
    #: Run inference.
    #: inputs: one item per input tensor. Each item is an ulab ndarray or a callable that fills the raw tensor buffer.
    #: callback: optional one-shot post-processing callback; overrides the default postprocess callback.
    def predict(
        self,
        inputs: Sequence[TensorInput | TensorWriter],
        *,
        callback: Postprocess | None = None,
    ) -> list[TensorOutput] | object: ...
