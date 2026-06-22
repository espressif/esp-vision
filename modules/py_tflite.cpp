/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * TensorFlow Lite for Microcontrollers inference module (`tflite`).
 *
 * This is a pure inference engine: it loads a `.tflite` flatbuffer, runs the
 * TFLM interpreter, and exposes the input/output tensors (with their
 * quantization parameters) to Python. Pre- and post-processing (image
 * normalization, YOLO decoding, NMS, ...) are intentionally kept out of this
 * module and live in MicroPython example/library code so they can be edited
 * without rebuilding firmware.
 *
 * Public API (usage-compatible with the OpenMV `ml` module):
 *   import tflite
 *   m = tflite.Model("model.tflite", postprocess=None)
 *   out = m.predict([input])          # input: ulab ndarray or callable
 *   m.input_shape, m.input_scale, m.input_zero_point, m.input_dtype
 *   m.output_shape, m.output_scale, m.output_zero_point, m.output_dtype
 *   m.len   # model size in bytes
 *   m.ram   # tensor arena size in bytes
 */

#include <cstdint>
#include <cstring>
#include <new>

#include "esp_heap_caps.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

extern "C" {
#include "extmod/vfs.h"
#include "py/binary.h"
#include "py/mperrno.h"
#include "py/obj.h"
#include "py/objtuple.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "ulab/code/ndarray.h"
}

namespace {
constexpr size_t TFLITE_MODEL_ALIGN = 16;
constexpr size_t TFLITE_ARENA_ALIGN = 16;
// Extra arena headroom on top of the interpreter's reported optimal size.
constexpr size_t TFLITE_ARENA_EXTRA = 1024;
// Maximum size of the temporary probe arena used to discover the optimal arena
// size. Kept generous but bounded so we don't momentarily grab all of PSRAM.
constexpr size_t TFLITE_PROBE_MAX = 16 * 1024 * 1024;
// Number of operators registered in the resolver (must be >= the number of
// Add*() calls in tflite_register_ops()).
constexpr size_t TFLITE_OPS_COUNT = 96;
using TFLiteOpResolver = tflite::MicroMutableOpResolver<TFLITE_OPS_COUNT>;
}  // namespace

typedef struct _py_tflite_model_obj_t {
    mp_obj_base_t base;
    size_t size;            // model flatbuffer size in bytes
    uint8_t *model_data;    // aligned model buffer (heap_caps, not GC-managed)
    size_t arena_size;      // tensor arena size in bytes
    uint8_t *arena;         // tensor arena (heap_caps, not GC-managed)

    const tflite::Model *model;
    TFLiteOpResolver *resolver;
    tflite::MicroInterpreter *interpreter;

    size_t inputs_size;
    mp_obj_tuple_t *input_shape;
    mp_obj_tuple_t *input_scale;
    mp_obj_tuple_t *input_zero_point;
    mp_obj_tuple_t *input_dtype;       // tuple of int char-codes ('b'/'B'/'h'/'H'/'f')

    size_t outputs_size;
    mp_obj_tuple_t *output_shape;
    mp_obj_tuple_t *output_scale;
    mp_obj_tuple_t *output_zero_point;
    mp_obj_tuple_t *output_dtype;

    mp_obj_t postprocess;              // optional post-processing callable
} py_tflite_model_obj_t;

extern const mp_obj_type_t py_tflite_model_type;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint8_t *tflite_caps_alloc(size_t size, size_t align)
{
    uint8_t *p = (uint8_t *) heap_caps_aligned_alloc(align, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) {
        p = (uint8_t *) heap_caps_aligned_alloc(align, size, MALLOC_CAP_DEFAULT);
    }
    return p;
}

static bool tflite_valid_dtype(TfLiteType type)
{
    return (type == kTfLiteUInt8 ||
            type == kTfLiteInt8 ||
            type == kTfLiteUInt16 ||
            type == kTfLiteInt16 ||
            type == kTfLiteFloat32);
}

// Map a TFLite tensor type to a ulab/struct dtype char-code.
static char tflite_dtype_char(TfLiteType type)
{
    switch (type) {
    case kTfLiteUInt8:  return 'B';
    case kTfLiteInt8:   return 'b';
    case kTfLiteUInt16: return 'H';
    case kTfLiteInt16:  return 'h';
    default:            return 'f';
    }
}

static size_t tflite_dtype_size(char dtype)
{
    switch (dtype) {
    case 'f': return 4;
    case 'H':
    case 'h': return 2;
    default:  return 1;
    }
}

static size_t tflite_shape_elements(const mp_obj_tuple_t *shape)
{
    if (shape->len < 1) {
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("Unexpected tensor shape"));
    }
    size_t n = mp_obj_get_int(shape->items[0]);
    for (size_t i = 1; i < shape->len; i++) {
        n *= mp_obj_get_int(shape->items[i]);
    }
    return n;
}

static void tflite_register_ops(TFLiteOpResolver *r)
{
    // Convolution / dense
    r->AddConv2D();
    r->AddDepthwiseConv2D();
    r->AddTransposeConv();
    r->AddFullyConnected();
    r->AddBatchMatMul();
    // Pooling
    r->AddAveragePool2D();
    r->AddMaxPool2D();
    r->AddL2Pool2D();
    // Activations
    r->AddRelu();
    r->AddRelu6();
    r->AddLeakyRelu();
    r->AddPrelu();
    r->AddElu();
    r->AddHardSwish();
    r->AddLogistic();
    r->AddTanh();
    r->AddSoftmax();
    r->AddLogSoftmax();
    r->AddL2Normalization();
    // Element-wise arithmetic
    r->AddAdd();
    r->AddAddN();
    r->AddSub();
    r->AddMul();
    r->AddDiv();
    r->AddNeg();
    r->AddAbs();
    r->AddSqrt();
    r->AddRsqrt();
    r->AddSquare();
    r->AddSquaredDifference();
    r->AddExp();
    r->AddLog();
    r->AddFloor();
    r->AddCeil();
    r->AddRound();
    r->AddFloorDiv();
    r->AddFloorMod();
    r->AddMaximum();
    r->AddMinimum();
    r->AddCos();
    r->AddSin();
    // Reductions
    r->AddMean();
    r->AddSum();
    r->AddReduceMax();
    r->AddReduceMin();
    r->AddArgMax();
    r->AddArgMin();
    // Comparison / logical
    r->AddEqual();
    r->AddNotEqual();
    r->AddGreater();
    r->AddGreaterEqual();
    r->AddLess();
    r->AddLessEqual();
    r->AddLogicalAnd();
    r->AddLogicalOr();
    r->AddLogicalNot();
    r->AddSelectV2();
    // Shape / layout
    r->AddReshape();
    r->AddSqueeze();
    r->AddExpandDims();
    r->AddShape();
    r->AddPack();
    r->AddUnpack();
    r->AddConcatenation();
    r->AddSplit();
    r->AddSplitV();
    r->AddSlice();
    r->AddStridedSlice();
    r->AddPad();
    r->AddPadV2();
    r->AddMirrorPad();
    r->AddTranspose();
    r->AddGather();
    r->AddGatherNd();
    r->AddSpaceToDepth();
    r->AddDepthToSpace();
    r->AddSpaceToBatchNd();
    r->AddBatchToSpaceNd();
    r->AddReverseV2();
    r->AddFill();
    r->AddZerosLike();
    r->AddBroadcastTo();
    r->AddBroadcastArgs();
    // Resize
    r->AddResizeBilinear();
    r->AddResizeNearestNeighbor();
    // Quantization
    r->AddQuantize();
    r->AddDequantize();
    r->AddCast();
    // Detection post-processing op (used by some SSD/MobileNet exports)
    r->AddDetectionPostprocess();
}

// Build a tuple of single-char dtype strings from the stored int char-codes.
static mp_obj_t tflite_dtype_str_tuple(const mp_obj_tuple_t *dtypes)
{
    mp_obj_tuple_t *out = (mp_obj_tuple_t *) MP_OBJ_TO_PTR(mp_obj_new_tuple(dtypes->len, NULL));
    for (size_t i = 0; i < dtypes->len; i++) {
        char c = (char) mp_obj_get_int(dtypes->items[i]);
        out->items[i] = mp_obj_new_str(&c, 1);
    }
    return MP_OBJ_FROM_PTR(out);
}

// ---------------------------------------------------------------------------
// Model construction
// ---------------------------------------------------------------------------

static void tflite_cache_tensor_meta(py_tflite_model_obj_t *self)
{
    tflite::MicroInterpreter *interp = self->interpreter;

    self->inputs_size = interp->inputs_size();
    self->input_shape = (mp_obj_tuple_t *) MP_OBJ_TO_PTR(mp_obj_new_tuple(self->inputs_size, NULL));
    self->input_scale = (mp_obj_tuple_t *) MP_OBJ_TO_PTR(mp_obj_new_tuple(self->inputs_size, NULL));
    self->input_zero_point = (mp_obj_tuple_t *) MP_OBJ_TO_PTR(mp_obj_new_tuple(self->inputs_size, NULL));
    self->input_dtype = (mp_obj_tuple_t *) MP_OBJ_TO_PTR(mp_obj_new_tuple(self->inputs_size, NULL));

    for (size_t i = 0; i < self->inputs_size; i++) {
        TfLiteTensor *t = interp->input(i);
        if (!tflite_valid_dtype(t->type)) {
            mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("Unsupported input data type %d"), t->type);
        }
        mp_obj_tuple_t *shape = (mp_obj_tuple_t *) MP_OBJ_TO_PTR(mp_obj_new_tuple(t->dims->size, NULL));
        for (int j = 0; j < t->dims->size; j++) {
            shape->items[j] = mp_obj_new_int(t->dims->data[j]);
        }
        float scale = t->params.scale;
        self->input_shape->items[i] = MP_OBJ_FROM_PTR(shape);
        self->input_scale->items[i] = mp_obj_new_float((scale == 0.0f) ? 1.0f : scale);
        self->input_zero_point->items[i] = mp_obj_new_int(t->params.zero_point);
        self->input_dtype->items[i] = mp_obj_new_int(tflite_dtype_char(t->type));
    }

    self->outputs_size = interp->outputs_size();
    self->output_shape = (mp_obj_tuple_t *) MP_OBJ_TO_PTR(mp_obj_new_tuple(self->outputs_size, NULL));
    self->output_scale = (mp_obj_tuple_t *) MP_OBJ_TO_PTR(mp_obj_new_tuple(self->outputs_size, NULL));
    self->output_zero_point = (mp_obj_tuple_t *) MP_OBJ_TO_PTR(mp_obj_new_tuple(self->outputs_size, NULL));
    self->output_dtype = (mp_obj_tuple_t *) MP_OBJ_TO_PTR(mp_obj_new_tuple(self->outputs_size, NULL));

    for (size_t i = 0; i < self->outputs_size; i++) {
        TfLiteTensor *t = interp->output(i);
        if (!tflite_valid_dtype(t->type)) {
            mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("Unsupported output data type %d"), t->type);
        }
        mp_obj_tuple_t *shape = (mp_obj_tuple_t *) MP_OBJ_TO_PTR(mp_obj_new_tuple(t->dims->size, NULL));
        for (int j = 0; j < t->dims->size; j++) {
            shape->items[j] = mp_obj_new_int(t->dims->data[j]);
        }
        float scale = t->params.scale;
        self->output_shape->items[i] = MP_OBJ_FROM_PTR(shape);
        self->output_scale->items[i] = mp_obj_new_float((scale == 0.0f) ? 1.0f : scale);
        self->output_zero_point->items[i] = mp_obj_new_int(t->params.zero_point);
        self->output_dtype->items[i] = mp_obj_new_int(tflite_dtype_char(t->type));
    }
}

static void tflite_init_model(py_tflite_model_obj_t *self)
{
    self->model = tflite::GetModel(self->model_data);
    if (self->model->version() != TFLITE_SCHEMA_VERSION) {
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("Unsupported TFLite model schema version"));
    }

    self->resolver = new (std::nothrow) TFLiteOpResolver();
    if (self->resolver == nullptr) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("Failed to allocate op resolver"));
    }
    tflite_register_ops(self->resolver);

    // Pass 1: build a throw-away interpreter in a large probe arena to learn
    // the optimal arena size for this model.
    size_t probe_size = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (probe_size == 0) {
        probe_size = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    }
    if (probe_size > TFLITE_PROBE_MAX) {
        probe_size = TFLITE_PROBE_MAX;
    }
    if (probe_size > 4096) {
        probe_size -= 4096;  // leave headroom for allocator bookkeeping
    }
    uint8_t *probe = tflite_caps_alloc(probe_size, TFLITE_ARENA_ALIGN);
    if (probe == nullptr) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("Failed to allocate probe arena"));
    }
    {
        tflite::MicroInterpreter probe_interp(self->model, *self->resolver, probe, probe_size);
        if (probe_interp.AllocateTensors() != kTfLiteOk) {
            heap_caps_free(probe);
            mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("Failed to allocate tensors (model too large or unsupported op)"));
        }
        size_t used = probe_interp.arena_used_bytes();
        self->arena_size = ((used + TFLITE_ARENA_ALIGN - 1) & ~(TFLITE_ARENA_ALIGN - 1)) + TFLITE_ARENA_EXTRA;
    }
    heap_caps_free(probe);

    // Pass 2: build the persistent interpreter in an exactly-sized arena.
    self->arena = tflite_caps_alloc(self->arena_size, TFLITE_ARENA_ALIGN);
    if (self->arena == nullptr) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("Failed to allocate tensor arena"));
    }
    self->interpreter = new (std::nothrow) tflite::MicroInterpreter(self->model, *self->resolver,
                                                                    self->arena, self->arena_size);
    if (self->interpreter == nullptr) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("Failed to allocate interpreter"));
    }
    if (self->interpreter->AllocateTensors() != kTfLiteOk) {
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("Failed to allocate tensors"));
    }

    tflite_cache_tensor_meta(self);
}

// ---------------------------------------------------------------------------
// Inference
// ---------------------------------------------------------------------------

static void tflite_process_input(py_tflite_model_obj_t *self, mp_obj_t arg)
{
    if (!mp_obj_is_type(arg, &mp_type_list)) {
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("Expected a list of inputs"));
    }
    mp_obj_list_t *inputs = (mp_obj_list_t *) MP_OBJ_TO_PTR(arg);
    if (inputs->len != self->inputs_size) {
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("Wrong number of inputs"));
    }

    for (size_t i = 0; i < self->inputs_size; i++) {
        void *buffer = self->interpreter->input(i)->data.data;
        mp_obj_tuple_t *shape = (mp_obj_tuple_t *) MP_OBJ_TO_PTR(self->input_shape->items[i]);
        size_t elements = tflite_shape_elements(shape);
        int dtype = mp_obj_get_int(self->input_dtype->items[i]);
        float inv_scale = 1.0f / mp_obj_get_float_to_f(self->input_scale->items[i]);
        int zero_point = mp_obj_get_int(self->input_zero_point->items[i]);
        mp_obj_t item = inputs->items[i];

        if (mp_obj_is_callable(item)) {
            // The callable fills the raw tensor buffer itself.
            mp_obj_t fargs[3] = {
                mp_obj_new_bytearray_by_ref(elements * tflite_dtype_size(dtype), buffer),
                MP_OBJ_FROM_PTR(shape),
                mp_obj_new_int(dtype),
            };
            mp_call_function_n_kw(item, 3, 0, fargs);
            continue;
        }

        if (!mp_obj_is_type(item, &ulab_ndarray_type)) {
            mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("Input must be an ndarray or a callable"));
        }
        ndarray_obj_t *src = (ndarray_obj_t *) MP_OBJ_TO_PTR(item);
        if (src->len != elements) {
            mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("Input shape does not match model input"));
        }

        switch (dtype) {
        case 'f': {
            float *dst = (float *) buffer;
            for (size_t k = 0; k < elements; k++) {
                dst[k] = ndarray_get_float_index(src->array, src->dtype, k);
            }
            break;
        }
        case 'b': {
            int8_t *dst = (int8_t *) buffer;
            for (size_t k = 0; k < elements; k++) {
                dst[k] = (int8_t)(ndarray_get_float_index(src->array, src->dtype, k) * inv_scale + zero_point);
            }
            break;
        }
        case 'B': {
            uint8_t *dst = (uint8_t *) buffer;
            for (size_t k = 0; k < elements; k++) {
                dst[k] = (uint8_t)(ndarray_get_float_index(src->array, src->dtype, k) * inv_scale + zero_point);
            }
            break;
        }
        case 'h': {
            int16_t *dst = (int16_t *) buffer;
            for (size_t k = 0; k < elements; k++) {
                dst[k] = (int16_t)(ndarray_get_float_index(src->array, src->dtype, k) * inv_scale + zero_point);
            }
            break;
        }
        case 'H': {
            uint16_t *dst = (uint16_t *) buffer;
            for (size_t k = 0; k < elements; k++) {
                dst[k] = (uint16_t)(ndarray_get_float_index(src->array, src->dtype, k) * inv_scale + zero_point);
            }
            break;
        }
        default:
            mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("Unsupported input data type"));
        }
    }
}

// Build the output list of ndarrays. Each output keeps its native dtype and
// raw (still-quantized) values; callers dequantize using output_scale /
// output_zero_point as needed.
static mp_obj_t tflite_process_output(py_tflite_model_obj_t *self)
{
    mp_obj_list_t *out = (mp_obj_list_t *) MP_OBJ_TO_PTR(mp_obj_new_list(self->outputs_size, NULL));
    for (size_t i = 0; i < self->outputs_size; i++) {
        TfLiteTensor *t = self->interpreter->output(i);
        mp_obj_tuple_t *shape_tuple = (mp_obj_tuple_t *) MP_OBJ_TO_PTR(self->output_shape->items[i]);
        uint8_t ndim = shape_tuple->len;
        size_t shape[ULAB_MAX_DIMS] = {};

        if (ndim > ULAB_MAX_DIMS) {
            ndim = 1;
            shape[ULAB_MAX_DIMS - 1] = tflite_shape_elements(shape_tuple);
        } else {
            for (uint8_t j = 0; j < ndim; j++) {
                shape[ULAB_MAX_DIMS - ndim + j] = mp_obj_get_int(shape_tuple->items[j]);
            }
        }

        char dtype = (char) mp_obj_get_int(self->output_dtype->items[i]);
        ndarray_obj_t *arr = ndarray_new_dense_ndarray(ndim, shape, (uint8_t) dtype);
        memcpy(arr->array, t->data.data, arr->len * arr->itemsize);
        out->items[i] = MP_OBJ_FROM_PTR(arr);
    }
    return MP_OBJ_FROM_PTR(out);
}

static mp_obj_t py_tflite_model_predict(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum { ARG_callback };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_callback, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 2, pos_args + 2, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    py_tflite_model_obj_t *self = (py_tflite_model_obj_t *) MP_OBJ_TO_PTR(pos_args[0]);

    mp_obj_t callback = args[ARG_callback].u_obj;
    bool post = (callback != mp_const_none) || (self->postprocess != mp_const_none);

    tflite_process_input(self, pos_args[1]);
    if (self->interpreter->Invoke() != kTfLiteOk) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Inference failed"));
    }
    mp_obj_t output = tflite_process_output(self);

    if (post) {
        mp_obj_t fargs[3] = { MP_OBJ_FROM_PTR(self), pos_args[1], output };
        mp_obj_t fn = (callback != mp_const_none) ? callback : self->postprocess;
        output = mp_call_function_n_kw(fn, 3, 0, fargs);
    }
    return output;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(py_tflite_model_predict_obj, 2, py_tflite_model_predict);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static mp_obj_t py_tflite_model_deinit(mp_obj_t self_in)
{
    py_tflite_model_obj_t *self = (py_tflite_model_obj_t *) MP_OBJ_TO_PTR(self_in);
    if (self->interpreter != nullptr) {
        delete self->interpreter;
        self->interpreter = nullptr;
    }
    if (self->resolver != nullptr) {
        delete self->resolver;
        self->resolver = nullptr;
    }
    if (self->arena != nullptr) {
        heap_caps_free(self->arena);
        self->arena = nullptr;
    }
    if (self->model_data != nullptr) {
        heap_caps_free(self->model_data);
        self->model_data = nullptr;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(py_tflite_model_deinit_obj, py_tflite_model_deinit);

static mp_obj_t py_tflite_model_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw,
                                         const mp_obj_t *all_args)
{
    enum { ARG_path, ARG_postprocess };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_path, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_postprocess, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_rom_obj = MP_ROM_NONE} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    py_tflite_model_obj_t *self =
        mp_obj_malloc_with_finaliser(py_tflite_model_obj_t, (const mp_obj_type_t *) &py_tflite_model_type);
    self->model_data = nullptr;
    self->arena = nullptr;
    self->resolver = nullptr;
    self->interpreter = nullptr;
    self->postprocess = args[ARG_postprocess].u_obj;

    // Read the model file into a 16-byte aligned heap_caps buffer.
    mp_obj_t open_args[2] = { args[ARG_path].u_obj, MP_OBJ_NEW_QSTR(MP_QSTR_rb) };
    mp_obj_t file = mp_vfs_open(MP_ARRAY_SIZE(open_args), open_args, (mp_map_t *) &mp_const_empty_map);

    int err;
    mp_off_t fsize = mp_stream_seek(file, 0, MP_SEEK_END, &err);
    if (fsize == (mp_off_t) -1) {
        mp_stream_close(file);
        mp_raise_OSError(err);
    }
    if (mp_stream_seek(file, 0, MP_SEEK_SET, &err) == (mp_off_t) -1) {
        mp_stream_close(file);
        mp_raise_OSError(err);
    }
    self->size = (size_t) fsize;
    self->model_data = tflite_caps_alloc(self->size, TFLITE_MODEL_ALIGN);
    if (self->model_data == nullptr) {
        mp_stream_close(file);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("Failed to allocate model buffer"));
    }
    mp_stream_read_exactly(file, self->model_data, self->size, &err);
    mp_stream_close(file);
    if (err != 0) {
        mp_raise_OSError(err);
    }

    tflite_init_model(self);
    return MP_OBJ_FROM_PTR(self);
}

// ---------------------------------------------------------------------------
// Attributes & type
// ---------------------------------------------------------------------------

static void py_tflite_model_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest)
{
    py_tflite_model_obj_t *self = (py_tflite_model_obj_t *) MP_OBJ_TO_PTR(self_in);
    if (dest[0] != MP_OBJ_NULL) {
        return;  // read-only attributes
    }
    switch (attr) {
    case MP_QSTR_len:               dest[0] = mp_obj_new_int(self->size); break;
    case MP_QSTR_ram:               dest[0] = mp_obj_new_int(self->arena_size); break;
    case MP_QSTR_input_shape:       dest[0] = MP_OBJ_FROM_PTR(self->input_shape); break;
    case MP_QSTR_input_scale:       dest[0] = MP_OBJ_FROM_PTR(self->input_scale); break;
    case MP_QSTR_input_zero_point:  dest[0] = MP_OBJ_FROM_PTR(self->input_zero_point); break;
    case MP_QSTR_input_dtype:       dest[0] = tflite_dtype_str_tuple(self->input_dtype); break;
    case MP_QSTR_output_shape:      dest[0] = MP_OBJ_FROM_PTR(self->output_shape); break;
    case MP_QSTR_output_scale:      dest[0] = MP_OBJ_FROM_PTR(self->output_scale); break;
    case MP_QSTR_output_zero_point: dest[0] = MP_OBJ_FROM_PTR(self->output_zero_point); break;
    case MP_QSTR_output_dtype:      dest[0] = tflite_dtype_str_tuple(self->output_dtype); break;
    case MP_QSTR_postprocess:       dest[0] = self->postprocess; break;
    default:                        dest[1] = MP_OBJ_SENTINEL; break;  // continue in locals_dict
    }
}

static const mp_rom_map_elem_t py_tflite_model_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&py_tflite_model_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit),  MP_ROM_PTR(&py_tflite_model_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_predict), MP_ROM_PTR(&py_tflite_model_predict_obj) },
};
static MP_DEFINE_CONST_DICT(py_tflite_model_locals_dict, py_tflite_model_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    py_tflite_model_type,
    MP_QSTR_Model,
    MP_TYPE_FLAG_NONE,
    make_new, reinterpret_cast<const void *>(py_tflite_model_make_new),
    attr, reinterpret_cast<const void *>(py_tflite_model_attr),
    locals_dict, &py_tflite_model_locals_dict
);

static const mp_rom_map_elem_t tflite_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_tflite) },
    { MP_ROM_QSTR(MP_QSTR_Model),    MP_ROM_PTR(&py_tflite_model_type) },
};
static MP_DEFINE_CONST_DICT(tflite_module_globals, tflite_module_globals_table);

extern "C" const mp_obj_module_t tflite_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *) &tflite_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_tflite, tflite_module);
