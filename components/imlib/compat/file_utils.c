/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2013-2024 OpenMV, LLC.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * Filesystem helper functions using MicroPython VFS interface.
 */
#include "imlib_config.h"
#if defined(IMLIB_ENABLE_IMAGE_FILE_IO)

#include <string.h>
#include <unistd.h>
#include "py/runtime.h"
#include "py/stream.h"
#include "py/builtin.h"
#include "extmod/vfs.h"

#include "omv_common.h"
#include "fb_alloc.h"
#include "file_utils.h"
#define FF_MIN(x, y)    (((x) < (y))?(x):(y))

// File buffering support
static file_t *file_buffer_owner = NULL;
static uint32_t file_buffer_offset = 0;
static uint8_t *file_buffer_pointer = 0;
static uint32_t file_buffer_size = 0;
static uint32_t file_buffer_index = 0;

static void file_buffer_discard(file_t *fp) {
    if (file_buffer_owner == fp && file_buffer_pointer != NULL) {
        uint8_t *allocation = file_buffer_pointer - file_buffer_offset;
        // Unregister before freeing: exception cleanup must never flush this data.
        file_buffer_init0();
        fb_free_buffer(allocation);
    }
}

void file_abort(file_t *fp) {
    if (fp == NULL) {
        return;
    }
    file_buffer_discard(fp);
    mp_obj_t stream = fp->fp;
    fp->fp = MP_OBJ_NULL;
    if (stream != MP_OBJ_NULL) {
        // Closing a VFS stream may itself raise. Preserve the original error and
        // make repeated cleanup harmless, including nested file guards.
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            mp_stream_close(stream);
            nlr_pop();
        }
    }
}

static void file_guard_cleanup(void *ctx) {
    file_abort(((file_guard_t *) ctx)->fp);
}

void file_guard_begin(file_guard_t *guard, file_t *fp) {
    guard->fp = fp;
    nlr_push_jump_callback(&guard->callback, file_guard_cleanup);
}

// Error/exception functions
NORETURN static void file_read_fail(file_t *fp) {
    file_abort(fp);
    mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Failed to read requested bytes!"));
}

NORETURN static void file_write_fail(file_t *fp) {
    file_abort(fp);
    mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Failed to write requested bytes!"));
}

NORETURN static void file_expect_fail(file_t *fp) {
    file_abort(fp);
    mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Unexpected value read!"));
}

NORETURN void file_raise_format(file_t *fp) {
    file_abort(fp);
    mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Unsupported format!"));
}

NORETURN void file_raise_corrupted(file_t *fp) {
    file_abort(fp);
    mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("File corrupted!"));
}

NORETURN void file_raise_error(file_t *fp, mp_rom_error_text_t msg) {
    file_abort(fp);
    mp_raise_msg(&mp_type_OSError, msg);
}

// Helper function to perform stream seek
static off_t file_seek_helper(file_t *fp, off_t offset, int whence) {
    int err;
    const mp_stream_p_t *stream_p = mp_get_stream_raise(fp->fp, MP_STREAM_OP_IOCTL);
    struct mp_stream_seek_t seek_s;
    seek_s.offset = offset;
    seek_s.whence = whence;
    mp_uint_t res = stream_p->ioctl(fp->fp, MP_STREAM_SEEK, (mp_uint_t) (uintptr_t) &seek_s, &err);
    if (res == MP_STREAM_ERROR) {
        file_raise_error(fp, MP_ERROR_TEXT("Seek failed"));
    }
    return seek_s.offset;
}

// Helper function to read from stream
static size_t file_read_helper(file_t *fp, void *buf, size_t len) {
    int err;
    const mp_stream_p_t *stream_p = mp_get_stream_raise(fp->fp, MP_STREAM_OP_READ);
    mp_uint_t out_sz = stream_p->read(fp->fp, buf, len, &err);
    if (out_sz == MP_STREAM_ERROR) {
        file_raise_error(fp, MP_ERROR_TEXT("Read failed"));
    }
    return out_sz;
}

// Helper function to write to stream
static size_t file_write_helper(file_t *fp, const void *buf, size_t len) {
    int err;
    const mp_stream_p_t *stream_p = mp_get_stream_raise(fp->fp, MP_STREAM_OP_WRITE);
    mp_uint_t out_sz = stream_p->write(fp->fp, buf, len, &err);
    if (out_sz == MP_STREAM_ERROR) {
        file_raise_error(fp, MP_ERROR_TEXT("Write failed"));
    }
    return out_sz;
}

void file_buffer_init0() {
    file_buffer_owner = NULL;
    file_buffer_offset = 0;
    file_buffer_pointer = 0;
    file_buffer_size = 0;
    file_buffer_index = 0;
}

void file_buffer_on(file_t *fp) {
    if (!fp || fp->fp == MP_OBJ_NULL) {
        return;
    }

    if (file_buffer_owner != NULL) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("File buffer is already in use"));
    }
    file_guard_t guard;
    file_guard_begin(&guard, fp);
    off_t current_pos = file_seek_helper(fp, 0, SEEK_CUR);
    uint32_t offset = current_pos % 4;
    uint32_t size = 0;
    uint8_t *allocation = fb_alloc_all(&size, FB_ALLOC_PREFER_SIZE);
    if (allocation == NULL || size <= offset) {
        fb_free_buffer(allocation);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("No memory!"));
    }
    file_buffer_owner = fp;
    file_buffer_offset = offset;
    file_buffer_pointer = allocation + offset;
    file_buffer_size = size - offset;
    file_buffer_index = 0;

    if (fp->flags & FA_READ) {
        // Pre-fill buffer for reading
        off_t file_end = file_seek_helper(fp, 0, SEEK_END);
        file_seek_helper(fp, current_pos, SEEK_SET);
        uint32_t file_remaining = file_end - current_pos;
        uint32_t can_do = FF_MIN(file_buffer_size, file_remaining);

        size_t bytes = file_read_helper(fp, file_buffer_pointer, can_do);
        if (bytes != can_do) {
            file_read_fail(fp);
        }
    }
    nlr_pop_jump_callback(false);
}

void file_buffer_off(file_t *fp) {
    if (fp == NULL || file_buffer_owner != fp) {
        return;
    }
    file_guard_t guard;
    file_guard_begin(&guard, fp);
    if (file_buffer_index && fp->fp != MP_OBJ_NULL && (fp->flags & FA_WRITE)) {
        size_t bytes = file_write_helper(fp, file_buffer_pointer, file_buffer_index);
        if (bytes != file_buffer_index) {
            file_write_fail(fp);
        }
    }
    file_buffer_discard(fp);
    nlr_pop_jump_callback(false);
}

OMV_ATTR_ALWAYS_INLINE static void file_fill(file_t *fp) {
    if (file_buffer_index == file_buffer_size) {
        file_buffer_pointer -= file_buffer_offset;
        file_buffer_size += file_buffer_offset;
        file_buffer_offset = 0;
        file_buffer_index = 0;

        // Calculate remaining bytes in file
        off_t current_pos = file_seek_helper(fp, 0, SEEK_CUR);
        off_t file_end = file_seek_helper(fp, 0, SEEK_END);
        file_seek_helper(fp, current_pos, SEEK_SET);
        uint32_t file_remaining = file_end - current_pos;
        uint32_t can_do = FF_MIN(file_buffer_size, file_remaining);

        size_t bytes = file_read_helper(fp, file_buffer_pointer, can_do);
        if (bytes != can_do) {
            file_read_fail(fp);
        }
    }
}

OMV_ATTR_ALWAYS_INLINE static void file_flush(file_t *fp) {
    if (file_buffer_index == file_buffer_size) {
        size_t bytes = file_write_helper(fp, file_buffer_pointer, file_buffer_index);
        if (bytes != file_buffer_index) {
            file_write_fail(fp);
        }
        file_buffer_pointer -= file_buffer_offset;
        file_buffer_size += file_buffer_offset;
        file_buffer_offset = 0;
        file_buffer_index = 0;
    }
}

void file_open(file_t *fp, const char *path, bool buffered, uint32_t flags) {
    // Initialize fp to safe state in case open fails
    fp->fp = MP_OBJ_NULL;
    fp->flags = 0;
    file_guard_t guard;
    file_guard_begin(&guard, fp);

    // Reject unsupported FA_READ | FA_WRITE without creation/append flags
    // This combination would silently truncate the file (mode "wb")
    if ((flags & (FA_READ | FA_WRITE)) == (FA_READ | FA_WRITE)) {
        if (!(flags & (FA_CREATE_ALWAYS | FA_OPEN_APPEND | FA_OPEN_ALWAYS))) {
            mp_raise_msg(&mp_type_ValueError,
                         MP_ERROR_TEXT("FA_READ|FA_WRITE requires FA_OPEN_ALWAYS or FA_CREATE_ALWAYS"));
        }
    }

    // Convert flags to MicroPython mode string
    const char *mode;
    if (flags & FA_WRITE) {
        if (flags & FA_CREATE_ALWAYS) {
            mode = "wb";
        } else if (flags & FA_OPEN_APPEND) {
            mode = "ab";
        } else if (flags & FA_OPEN_ALWAYS) {
            mode = "r+b";
        } else {
            mode = "wb";
        }
    } else {
        mode = "rb";
    }

    // Open file using MicroPython VFS
    mp_obj_t args[2] = {
        mp_obj_new_str_from_cstr(path),
        mp_obj_new_str_from_cstr(mode)
    };
    fp->fp = mp_vfs_open(MP_ARRAY_SIZE(args), args, (mp_map_t *) &mp_const_empty_map);
    fp->flags = (uint8_t) flags;

    if (buffered) {
        file_buffer_on(fp);
    }
    nlr_pop_jump_callback(false);
}

void file_close(file_t *fp) {
    if (fp && fp->fp != MP_OBJ_NULL) {
        // file_buffer_off protects its flush and releases only this file's
        // buffer. Preserve the stream if close fails, so persistent ImageIO
        // files can retry; temporary files have an enclosing file guard.
        file_buffer_off(fp);
        mp_stream_close(fp->fp);
        fp->fp = MP_OBJ_NULL;
    }
}

void file_seek(file_t *fp, size_t offset) {
    if (fp && fp->fp) {
        file_seek_helper(fp, offset, SEEK_SET);
    }
}

void file_truncate(file_t *fp) {
    if (!fp || fp->fp == MP_OBJ_NULL) {
        return;
    }

    // Truncate file at current position by calling Python truncate() method
    // Use getattr to get the truncate method
    mp_obj_t truncate_str = mp_obj_new_str("truncate", 8);
    mp_obj_t truncate_method = mp_load_attr(fp->fp, qstr_from_str(mp_obj_str_get_str(truncate_str)));
    // Call truncate() method with no args (truncates at current position)
    mp_call_function_0(truncate_method);
}

void file_sync(file_t *fp) {
    int err;

    if (!fp || fp->fp == MP_OBJ_NULL) {
        return;
    }

    const mp_stream_p_t *stream_p = mp_get_stream_raise(fp->fp, MP_STREAM_OP_WRITE);
    mp_uint_t res = stream_p->ioctl(fp->fp, MP_STREAM_FLUSH, 0, &err);
    if (res == MP_STREAM_ERROR) {
        file_raise_error(fp, MP_ERROR_TEXT("Flush failed"));
    }
}

size_t file_tell(file_t *fp) {
    if (file_buffer_owner == fp && file_buffer_pointer) {
        off_t stream_pos = file_seek_helper(fp, 0, SEEK_CUR);
        if (fp->flags & FA_READ) {
            return stream_pos - file_buffer_size + file_buffer_index;
        } else {
            return stream_pos + file_buffer_index;
        }
    }
    return file_seek_helper(fp, 0, SEEK_CUR);
}

size_t file_size(file_t *fp) {
    if (file_buffer_owner == fp && file_buffer_pointer) {
        off_t current_pos = file_seek_helper(fp, 0, SEEK_CUR);
        off_t file_end = file_seek_helper(fp, 0, SEEK_END);
        file_seek_helper(fp, current_pos, SEEK_SET);

        if (fp->flags & FA_READ) {
            return file_end;
        } else {
            return file_end + file_buffer_index;
        }
    }
    off_t current_pos = file_seek_helper(fp, 0, SEEK_CUR);
    off_t file_end = file_seek_helper(fp, 0, SEEK_END);
    file_seek_helper(fp, current_pos, SEEK_SET);
    return file_end;
}

bool file_eof(file_t *fp) {
    if (file_buffer_owner == fp && file_buffer_pointer && (fp->flags & FA_READ)) {
        return file_buffer_index >= file_buffer_size;
    }
    off_t current_pos = file_seek_helper(fp, 0, SEEK_CUR);
    off_t file_end = file_seek_helper(fp, 0, SEEK_END);
    file_seek_helper(fp, current_pos, SEEK_SET);
    return current_pos >= file_end;
}

void file_read(file_t *fp, void *data, size_t size) {
    if (!fp || fp->fp == MP_OBJ_NULL) {
        return;
    }

    if (data == NULL) {
        // Skip bytes
        uint8_t byte;
        if (file_buffer_owner == fp && file_buffer_pointer) {
            for (size_t i = 0; i < size; i++) {
                file_fill(fp);
                byte = file_buffer_pointer[file_buffer_index++];
            }
        } else {
            for (size_t i = 0; i < size; i++) {
                size_t bytes = file_read_helper(fp, &byte, 1);
                if (bytes != 1) {
                    file_read_fail(fp);
                }
            }
        }
        return;
    }

    if (file_buffer_owner == fp && file_buffer_pointer) {
        if (size <= 4) {
            for (size_t i = 0; i < size; i++) {
                file_fill(fp);
                ((uint8_t *) data)[i] = file_buffer_pointer[file_buffer_index++];
            }
        } else {
            while (size) {
                file_fill(fp);
                uint32_t file_buffer_space_left = file_buffer_size - file_buffer_index;
                uint32_t can_do = FF_MIN(size, file_buffer_space_left);
                memcpy(data, file_buffer_pointer + file_buffer_index, can_do);
                file_buffer_index += can_do;
                data = ((uint8_t *) data) + can_do;
                size -= can_do;
            }
        }
    } else {
        size_t bytes = file_read_helper(fp, data, size);
        if (bytes != size) {
            file_read_fail(fp);
        }
    }
}

void file_write(file_t *fp, const void *data, size_t size) {
    if (!fp || fp->fp == MP_OBJ_NULL) {
        return;
    }

    if (file_buffer_owner == fp && file_buffer_pointer) {
        // Buffer writes for performance
        while (size) {
            uint32_t file_buffer_space_left = file_buffer_size - file_buffer_index;
            uint32_t can_do = FF_MIN(size, file_buffer_space_left);
            memcpy(file_buffer_pointer + file_buffer_index, data, can_do);
            file_buffer_index += can_do;
            data = ((const uint8_t *) data) + can_do;
            size -= can_do;
            file_flush(fp);
        }
    } else {
        size_t bytes = file_write_helper(fp, data, size);
        if (bytes != size) {
            file_write_fail(fp);
        }
    }
}

void file_write_byte(file_t *fp, uint8_t value) {
    file_write(fp, &value, 1);
}

void file_write_short(file_t *fp, uint16_t value) {
    file_write(fp, &value, 2);
}

void file_write_long(file_t *fp, uint32_t value) {
    file_write(fp, &value, 4);
}

void file_read_check(file_t *fp, const void *data, size_t size) {
    if (!fp || fp->fp == MP_OBJ_NULL) {
        return;
    }

    uint8_t buf[16];
    while (size) {
        size_t len = OMV_MIN(sizeof(buf), size);
        file_read(fp, buf, len);
        if (memcmp(data, buf, len)) {
            file_expect_fail(fp);
        }
        size -= len;
        data = ((const uint8_t *) data) + len;
    }
}

#endif //IMLIB_ENABLE_IMAGE_FILE_IO
