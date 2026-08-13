/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "storage.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "extmod/vfs.h"
#include "py/mperrno.h"
#include "py/mpthread.h"
#include "py/nlr.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "py/stream.h"

#define ESP_VISION_STORAGE_FULL_PATH_MAX (MICROPY_ALLOC_PATH_MAX + sizeof(ESP_VISION_STORAGE_SDCARD_PATH))

typedef struct {
    mp_obj_base_t base;
    const char *base_path;
    char cwd[MICROPY_ALLOC_PATH_MAX + 1];
    bool readonly;
} esp_vision_storage_vfs_obj_t;

typedef struct {
    mp_obj_base_t base;
    mp_fun_1_t iternext;
    mp_fun_1_t finaliser;
    DIR *dir;
    bool is_str;
} esp_vision_storage_dir_iter_t;

typedef struct {
    mp_obj_base_t base;
    int fd;
} esp_vision_storage_file_obj_t;

static const char *TAG = "esp_vision_storage_vfs";
static bool s_initial_vm_mount_done;
extern const mp_obj_type_t esp_vision_storage_vfs_type;
extern const mp_obj_type_t esp_vision_storage_fileio_type;
extern const mp_obj_type_t esp_vision_storage_textio_type;

static void esp_vision_storage_file_check_open(esp_vision_storage_file_obj_t *self)
{
    if (self->fd < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("I/O operation on closed file"));
    }
}

static int esp_vision_storage_retry_close(int fd)
{
    int result;
    do {
        result = close(fd);
    } while (result < 0 && errno == EINTR);
    return result;
}

static mp_obj_t esp_vision_storage_file_open(const char *path, mp_obj_t mode_in)
{
    const char *mode = mp_obj_str_get_str(mode_in);
    int flags = O_RDONLY;
    const mp_obj_type_t *type = &esp_vision_storage_textio_type;

    while (*mode != '\0') {
        switch (*mode++) {
        case 'r':
            flags = (flags & ~O_ACCMODE) | O_RDONLY;
            break;
        case 'w':
            flags = (flags & ~O_ACCMODE) | O_WRONLY | O_CREAT | O_TRUNC;
            break;
        case 'a':
            flags = (flags & ~O_ACCMODE) | O_WRONLY | O_CREAT | O_APPEND;
            break;
        case 'x':
            flags = (flags & ~O_ACCMODE) | O_WRONLY | O_CREAT | O_EXCL;
            break;
        case '+':
            flags = (flags & ~O_ACCMODE) | O_RDWR;
            break;
        case 'b':
            type = &esp_vision_storage_fileio_type;
            break;
        case 't':
            type = &esp_vision_storage_textio_type;
            break;
        default:
            break;
        }
    }

    esp_vision_storage_file_obj_t *self = mp_obj_malloc_with_finaliser(
                                              esp_vision_storage_file_obj_t, type);
    self->fd = -1;

    int fd;
    for (;;) {
        MP_THREAD_GIL_EXIT();
        fd = open(path, flags, 0644);
        int saved_errno = errno;
        MP_THREAD_GIL_ENTER();
        if (fd >= 0) {
            break;
        }
        if (saved_errno == EINTR) {
            mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
            continue;
        }
        mp_raise_OSError(saved_errno);
    }
    self->fd = fd;
    return MP_OBJ_FROM_PTR(self);
}

static void esp_vision_storage_file_print(const mp_print_t *print,
                                          mp_obj_t self_in,
                                          mp_print_kind_t kind)
{
    (void)kind;
    esp_vision_storage_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<io.%s %d>", mp_obj_get_type_str(self_in), self->fd);
}

static mp_obj_t esp_vision_storage_file_fileno(mp_obj_t self_in)
{
    esp_vision_storage_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    esp_vision_storage_file_check_open(self);
    return MP_OBJ_NEW_SMALL_INT(self->fd);
}
static MP_DEFINE_CONST_FUN_OBJ_1(esp_vision_storage_file_fileno_obj, esp_vision_storage_file_fileno);

static mp_uint_t esp_vision_storage_file_read(mp_obj_t self_in,
                                              void *buffer,
                                              mp_uint_t size,
                                              int *error_code)
{
    esp_vision_storage_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    esp_vision_storage_file_check_open(self);

    ssize_t result;
    int saved_errno;
    do {
        MP_THREAD_GIL_EXIT();
        result = read(self->fd, buffer, size);
        saved_errno = result < 0 ? errno : 0;
        MP_THREAD_GIL_ENTER();
    } while (result < 0 && saved_errno == EINTR);
    *error_code = saved_errno;
    return result < 0 ? MP_STREAM_ERROR : (mp_uint_t)result;
}

static mp_uint_t esp_vision_storage_file_write(mp_obj_t self_in,
                                               const void *buffer,
                                               mp_uint_t size,
                                               int *error_code)
{
    esp_vision_storage_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    esp_vision_storage_file_check_open(self);

    ssize_t result;
    int saved_errno;
    do {
        MP_THREAD_GIL_EXIT();
        result = write(self->fd, buffer, size);
        saved_errno = result < 0 ? errno : 0;
        MP_THREAD_GIL_ENTER();
    } while (result < 0 && saved_errno == EINTR);
    *error_code = saved_errno;
    return result < 0 ? MP_STREAM_ERROR : (mp_uint_t)result;
}

static mp_uint_t esp_vision_storage_file_ioctl(mp_obj_t self_in,
                                               mp_uint_t request,
                                               uintptr_t argument,
                                               int *error_code)
{
    esp_vision_storage_file_obj_t *self = MP_OBJ_TO_PTR(self_in);

    switch (request) {
    case MP_STREAM_FLUSH: {
        esp_vision_storage_file_check_open(self);
        int result;
        int saved_errno;
        do {
            MP_THREAD_GIL_EXIT();
            result = fsync(self->fd);
            saved_errno = result < 0 ? errno : 0;
            MP_THREAD_GIL_ENTER();
        } while (result < 0 && saved_errno == EINTR);
        *error_code = saved_errno;
        return result < 0 ? MP_STREAM_ERROR : 0;
    }
    case MP_STREAM_SEEK: {
        esp_vision_storage_file_check_open(self);
        struct mp_stream_seek_t *seek = (struct mp_stream_seek_t *)argument;
        MP_THREAD_GIL_EXIT();
        off_t result = lseek(self->fd, seek->offset, seek->whence);
        int saved_errno = result < 0 ? errno : 0;
        MP_THREAD_GIL_ENTER();
        *error_code = saved_errno;
        if (result < 0) {
            return MP_STREAM_ERROR;
        }
        seek->offset = result;
        return 0;
    }
    case MP_STREAM_CLOSE:
        if (self->fd >= 0) {
            MP_THREAD_GIL_EXIT();
            esp_vision_storage_retry_close(self->fd);
            MP_THREAD_GIL_ENTER();
            self->fd = -1;
        }
        return 0;
    case MP_STREAM_GET_FILENO:
        esp_vision_storage_file_check_open(self);
        return self->fd;
    case MP_STREAM_POLL:
        esp_vision_storage_file_check_open(self);
        return argument & (MP_STREAM_POLL_RD | MP_STREAM_POLL_WR);
    default:
        *error_code = MP_EINVAL;
        return MP_STREAM_ERROR;
    }
}

static const mp_rom_map_elem_t esp_vision_storage_file_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_fileno), MP_ROM_PTR(&esp_vision_storage_file_fileno_obj)},
    {MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj)},
    {MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj)},
    {MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj)},
    {MP_ROM_QSTR(MP_QSTR_readlines), MP_ROM_PTR(&mp_stream_unbuffered_readlines_obj)},
    {MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj)},
    {MP_ROM_QSTR(MP_QSTR_seek), MP_ROM_PTR(&mp_stream_seek_obj)},
    {MP_ROM_QSTR(MP_QSTR_tell), MP_ROM_PTR(&mp_stream_tell_obj)},
    {MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&mp_stream_flush_obj)},
    {MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mp_stream_close_obj)},
    {MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mp_stream_close_obj)},
    {MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj)},
    {MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&mp_stream___exit___obj)},
};
static MP_DEFINE_CONST_DICT(esp_vision_storage_file_locals, esp_vision_storage_file_locals_table);

static const mp_stream_p_t esp_vision_storage_fileio_stream = {
    .read = esp_vision_storage_file_read,
    .write = esp_vision_storage_file_write,
    .ioctl = esp_vision_storage_file_ioctl,
};

static const mp_stream_p_t esp_vision_storage_textio_stream = {
    .read = esp_vision_storage_file_read,
    .write = esp_vision_storage_file_write,
    .ioctl = esp_vision_storage_file_ioctl,
    .is_text = true,
};

MP_DEFINE_CONST_OBJ_TYPE(
    esp_vision_storage_fileio_type,
    MP_QSTR_FileIO,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    print, esp_vision_storage_file_print,
    protocol, &esp_vision_storage_fileio_stream,
    locals_dict, &esp_vision_storage_file_locals
);

MP_DEFINE_CONST_OBJ_TYPE(
    esp_vision_storage_textio_type,
    MP_QSTR_TextIOWrapper,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    print, esp_vision_storage_file_print,
    protocol, &esp_vision_storage_textio_stream,
    locals_dict, &esp_vision_storage_file_locals
);

static void esp_vision_storage_raise_path_error(size_t required, size_t capacity)
{
    mp_raise_OSError(required >= capacity ? ENAMETOOLONG : MP_EINVAL);
}

static void esp_vision_storage_normalize_path(esp_vision_storage_vfs_obj_t *self,
                                              const char *path,
                                              size_t path_len,
                                              char *output,
                                              size_t output_size)
{
    if (memchr(path, '\0', path_len) != NULL) {
        esp_vision_storage_raise_path_error(0, output_size);
    }

    size_t output_len;
    size_t path_offset = 0;
    if (path_len > 0 && path[0] == '/') {
        output[0] = '/';
        output_len = 1;
        path_offset = 1;
    } else {
        output_len = strlen(self->cwd);
        if (output_len >= output_size) {
            esp_vision_storage_raise_path_error(output_len, output_size);
        }
        memcpy(output, self->cwd, output_len);
    }

    while (path_offset < path_len) {
        while (path_offset < path_len && path[path_offset] == '/') {
            path_offset++;
        }
        size_t component_start = path_offset;
        while (path_offset < path_len && path[path_offset] != '/') {
            path_offset++;
        }
        size_t component_len = path_offset - component_start;

        if (component_len == 0 ||
                (component_len == 1 && path[component_start] == '.')) {
            continue;
        }
        if (component_len == 2 && path[component_start] == '.' && path[component_start + 1] == '.') {
            while (output_len > 1 && output[output_len - 1] != '/') {
                output_len--;
            }
            if (output_len > 1) {
                output_len--;
            }
            continue;
        }

        size_t separator_len = output_len > 1 ? 1 : 0;
        size_t required = output_len + separator_len + component_len + 1;
        if (required > output_size) {
            esp_vision_storage_raise_path_error(required, output_size);
        }
        if (separator_len != 0) {
            output[output_len++] = '/';
        }
        memcpy(output + output_len, path + component_start, component_len);
        output_len += component_len;
    }

    output[output_len] = '\0';
}

static void esp_vision_storage_make_path_raw(esp_vision_storage_vfs_obj_t *self,
                                             const char *path,
                                             size_t path_len,
                                             char *internal_path,
                                             char *full_path)
{
    esp_vision_storage_normalize_path(self,
                                      path,
                                      path_len,
                                      internal_path,
                                      MICROPY_ALLOC_PATH_MAX + 1);

    size_t base_len = strlen(self->base_path);
    size_t internal_len = strlen(internal_path);
    size_t suffix_offset = internal_len == 1 ? 1 : 0;
    size_t required = base_len + internal_len - suffix_offset + 1;
    if (required > ESP_VISION_STORAGE_FULL_PATH_MAX) {
        esp_vision_storage_raise_path_error(required, ESP_VISION_STORAGE_FULL_PATH_MAX);
    }
    memcpy(full_path, self->base_path, base_len);
    memcpy(full_path + base_len, internal_path + suffix_offset, internal_len - suffix_offset);
    full_path[required - 1] = '\0';
}

static void esp_vision_storage_make_path(esp_vision_storage_vfs_obj_t *self,
                                         mp_obj_t path_obj,
                                         char *internal_path,
                                         char *full_path)
{
    size_t path_len;
    const char *path = (const char *)mp_obj_str_get_data(path_obj, &path_len);
    esp_vision_storage_make_path_raw(self, path, path_len, internal_path, full_path);
}

static void esp_vision_storage_require_writable(esp_vision_storage_vfs_obj_t *self)
{
    if (self->readonly) {
        mp_raise_OSError(MP_EROFS);
    }
}

static mp_obj_t esp_vision_storage_vfs_mount(mp_obj_t self_in, mp_obj_t readonly, mp_obj_t mkfs)
{
    esp_vision_storage_vfs_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (mp_obj_is_true(mkfs)) {
        mp_raise_OSError(MP_EPERM);
    }
    if (mp_obj_is_true(readonly)) {
        self->readonly = true;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(esp_vision_storage_vfs_mount_obj, esp_vision_storage_vfs_mount);

static mp_obj_t esp_vision_storage_vfs_umount(mp_obj_t self_in)
{
    (void)self_in;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(esp_vision_storage_vfs_umount_obj, esp_vision_storage_vfs_umount);

static mp_obj_t esp_vision_storage_vfs_open(mp_obj_t self_in, mp_obj_t path_in, mp_obj_t mode_in)
{
    esp_vision_storage_vfs_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *mode = mp_obj_str_get_str(mode_in);
    if (self->readonly &&
            (strchr(mode, 'w') != NULL || strchr(mode, 'a') != NULL ||
             strchr(mode, 'x') != NULL || strchr(mode, '+') != NULL)) {
        mp_raise_OSError(MP_EROFS);
    }

    char internal_path[MICROPY_ALLOC_PATH_MAX + 1];
    char full_path[ESP_VISION_STORAGE_FULL_PATH_MAX];
    esp_vision_storage_make_path(self, path_in, internal_path, full_path);

    return esp_vision_storage_file_open(full_path, mode_in);
}
static MP_DEFINE_CONST_FUN_OBJ_3(esp_vision_storage_vfs_open_obj, esp_vision_storage_vfs_open);

static mp_obj_t esp_vision_storage_vfs_chdir(mp_obj_t self_in, mp_obj_t path_in)
{
    esp_vision_storage_vfs_obj_t *self = MP_OBJ_TO_PTR(self_in);
    char internal_path[MICROPY_ALLOC_PATH_MAX + 1];
    char full_path[ESP_VISION_STORAGE_FULL_PATH_MAX];
    esp_vision_storage_make_path(self, path_in, internal_path, full_path);

    struct stat stat_buffer;
    if (stat(full_path, &stat_buffer) != 0) {
        mp_raise_OSError(errno);
    }
    if (!S_ISDIR(stat_buffer.st_mode)) {
        mp_raise_OSError(MP_ENOTDIR);
    }
    memcpy(self->cwd, internal_path, strlen(internal_path) + 1);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(esp_vision_storage_vfs_chdir_obj, esp_vision_storage_vfs_chdir);

static mp_obj_t esp_vision_storage_vfs_getcwd(mp_obj_t self_in)
{
    esp_vision_storage_vfs_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_str(self->cwd, strlen(self->cwd));
}
static MP_DEFINE_CONST_FUN_OBJ_1(esp_vision_storage_vfs_getcwd_obj, esp_vision_storage_vfs_getcwd);

static mp_obj_t esp_vision_storage_dir_iter_close(mp_obj_t self_in)
{
    esp_vision_storage_dir_iter_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->dir != NULL) {
        MP_THREAD_GIL_EXIT();
        closedir(self->dir);
        MP_THREAD_GIL_ENTER();
        self->dir = NULL;
    }
    return mp_const_none;
}

static mp_obj_t esp_vision_storage_dir_iter_next(mp_obj_t self_in)
{
    esp_vision_storage_dir_iter_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->dir == NULL) {
        return MP_OBJ_STOP_ITERATION;
    }

    for (;;) {
        errno = 0;
        MP_THREAD_GIL_EXIT();
        struct dirent *entry = readdir(self->dir);
        int saved_errno = errno;
        MP_THREAD_GIL_ENTER();
        if (entry == NULL) {
            esp_vision_storage_dir_iter_close(self_in);
            if (saved_errno != 0) {
                mp_raise_OSError(saved_errno);
            }
            return MP_OBJ_STOP_ITERATION;
        }
        if (entry->d_name[0] == '.' &&
                (entry->d_name[1] == '\0' ||
                 (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }

        mp_obj_tuple_t *tuple = MP_OBJ_TO_PTR(mp_obj_new_tuple(3, NULL));
        if (self->is_str) {
            tuple->items[0] = mp_obj_new_str_from_cstr(entry->d_name);
        } else {
            tuple->items[0] = mp_obj_new_bytes((const byte *)entry->d_name, strlen(entry->d_name));
        }
#ifdef _DIRENT_HAVE_D_TYPE
        if (entry->d_type == DT_DIR) {
            tuple->items[1] = MP_OBJ_NEW_SMALL_INT(MP_S_IFDIR);
        } else if (entry->d_type == DT_REG) {
            tuple->items[1] = MP_OBJ_NEW_SMALL_INT(MP_S_IFREG);
        } else {
            tuple->items[1] = MP_OBJ_NEW_SMALL_INT(0);
        }
#else
        tuple->items[1] = MP_OBJ_NEW_SMALL_INT(0);
#endif
        tuple->items[2] = MP_OBJ_NEW_SMALL_INT(0);
        return MP_OBJ_FROM_PTR(tuple);
    }
}

static mp_obj_t esp_vision_storage_vfs_ilistdir(mp_obj_t self_in, mp_obj_t path_in)
{
    esp_vision_storage_vfs_obj_t *self = MP_OBJ_TO_PTR(self_in);
    char internal_path[MICROPY_ALLOC_PATH_MAX + 1];
    char full_path[ESP_VISION_STORAGE_FULL_PATH_MAX];
    esp_vision_storage_make_path(self, path_in, internal_path, full_path);

    esp_vision_storage_dir_iter_t *iter = mp_obj_malloc_with_finaliser(
                                              esp_vision_storage_dir_iter_t, &mp_type_polymorph_iter_with_finaliser);
    iter->iternext = esp_vision_storage_dir_iter_next;
    iter->finaliser = esp_vision_storage_dir_iter_close;
    iter->dir = NULL;
    iter->is_str = mp_obj_get_type(path_in) == &mp_type_str;

    MP_THREAD_GIL_EXIT();
    iter->dir = opendir(full_path);
    int saved_errno = errno;
    MP_THREAD_GIL_ENTER();
    if (iter->dir == NULL) {
        mp_raise_OSError(saved_errno);
    }
    return MP_OBJ_FROM_PTR(iter);
}
static MP_DEFINE_CONST_FUN_OBJ_2(esp_vision_storage_vfs_ilistdir_obj, esp_vision_storage_vfs_ilistdir);

static mp_obj_t esp_vision_storage_vfs_path_call(esp_vision_storage_vfs_obj_t *self,
                                                 mp_obj_t path_in,
                                                 int (*function)(const char *))
{
    char internal_path[MICROPY_ALLOC_PATH_MAX + 1];
    char full_path[ESP_VISION_STORAGE_FULL_PATH_MAX];
    esp_vision_storage_make_path(self, path_in, internal_path, full_path);

    MP_THREAD_GIL_EXIT();
    int result = function(full_path);
    int saved_errno = errno;
    MP_THREAD_GIL_ENTER();
    if (result != 0) {
        mp_raise_OSError(saved_errno);
    }
    return mp_const_none;
}

static int esp_vision_storage_mkdir_posix(const char *path)
{
    return mkdir(path, 0777);
}

static mp_obj_t esp_vision_storage_vfs_mkdir(mp_obj_t self_in, mp_obj_t path_in)
{
    esp_vision_storage_vfs_obj_t *self = MP_OBJ_TO_PTR(self_in);
    esp_vision_storage_require_writable(self);
    return esp_vision_storage_vfs_path_call(self, path_in, esp_vision_storage_mkdir_posix);
}
static MP_DEFINE_CONST_FUN_OBJ_2(esp_vision_storage_vfs_mkdir_obj, esp_vision_storage_vfs_mkdir);

static mp_obj_t esp_vision_storage_vfs_remove(mp_obj_t self_in, mp_obj_t path_in)
{
    esp_vision_storage_vfs_obj_t *self = MP_OBJ_TO_PTR(self_in);
    esp_vision_storage_require_writable(self);
    return esp_vision_storage_vfs_path_call(self, path_in, unlink);
}
static MP_DEFINE_CONST_FUN_OBJ_2(esp_vision_storage_vfs_remove_obj, esp_vision_storage_vfs_remove);

static mp_obj_t esp_vision_storage_vfs_rmdir(mp_obj_t self_in, mp_obj_t path_in)
{
    esp_vision_storage_vfs_obj_t *self = MP_OBJ_TO_PTR(self_in);
    esp_vision_storage_require_writable(self);
    return esp_vision_storage_vfs_path_call(self, path_in, rmdir);
}
static MP_DEFINE_CONST_FUN_OBJ_2(esp_vision_storage_vfs_rmdir_obj, esp_vision_storage_vfs_rmdir);

static mp_obj_t esp_vision_storage_vfs_rename(mp_obj_t self_in,
                                              mp_obj_t old_path_in,
                                              mp_obj_t new_path_in)
{
    esp_vision_storage_vfs_obj_t *self = MP_OBJ_TO_PTR(self_in);
    esp_vision_storage_require_writable(self);

    char old_internal[MICROPY_ALLOC_PATH_MAX + 1];
    char old_full[ESP_VISION_STORAGE_FULL_PATH_MAX];
    char new_internal[MICROPY_ALLOC_PATH_MAX + 1];
    char new_full[ESP_VISION_STORAGE_FULL_PATH_MAX];
    esp_vision_storage_make_path(self, old_path_in, old_internal, old_full);
    esp_vision_storage_make_path(self, new_path_in, new_internal, new_full);

    MP_THREAD_GIL_EXIT();
    int result;
    int saved_errno;
    do {
        result = rename(old_full, new_full);
        saved_errno = result < 0 ? errno : 0;
    } while (result < 0 && saved_errno == EINTR);

    // FatFS reports FR_EXIST when the destination already exists, while
    // MicroPython's VFS rename semantics replace an existing regular file.
    // Keep directories protected and retry after removing only a file target.
    if (result < 0 && saved_errno == EEXIST && strcmp(old_full, new_full) != 0) {
        struct stat new_stat;
        if (stat(new_full, &new_stat) == 0 && S_ISREG(new_stat.st_mode)) {
            int unlink_result;
            do {
                unlink_result = unlink(new_full);
                saved_errno = unlink_result < 0 ? errno : 0;
            } while (unlink_result < 0 && saved_errno == EINTR);

            if (unlink_result == 0) {
                do {
                    result = rename(old_full, new_full);
                    saved_errno = result < 0 ? errno : 0;
                } while (result < 0 && saved_errno == EINTR);
            } else {
                result = -1;
            }
        }
    }
    MP_THREAD_GIL_ENTER();
    if (result != 0) {
        mp_raise_OSError(saved_errno);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(esp_vision_storage_vfs_rename_obj, esp_vision_storage_vfs_rename);

static mp_obj_t esp_vision_storage_vfs_stat(mp_obj_t self_in, mp_obj_t path_in)
{
    esp_vision_storage_vfs_obj_t *self = MP_OBJ_TO_PTR(self_in);
    char internal_path[MICROPY_ALLOC_PATH_MAX + 1];
    char full_path[ESP_VISION_STORAGE_FULL_PATH_MAX];
    esp_vision_storage_make_path(self, path_in, internal_path, full_path);

    struct stat stat_buffer;
    if (stat(full_path, &stat_buffer) != 0) {
        mp_raise_OSError(errno);
    }

    mp_obj_tuple_t *tuple = MP_OBJ_TO_PTR(mp_obj_new_tuple(10, NULL));
    tuple->items[0] = MP_OBJ_NEW_SMALL_INT(stat_buffer.st_mode);
    tuple->items[1] = mp_obj_new_int_from_uint(stat_buffer.st_ino);
    tuple->items[2] = mp_obj_new_int_from_uint(stat_buffer.st_dev);
    tuple->items[3] = mp_obj_new_int_from_uint(stat_buffer.st_nlink);
    tuple->items[4] = mp_obj_new_int_from_uint(stat_buffer.st_uid);
    tuple->items[5] = mp_obj_new_int_from_uint(stat_buffer.st_gid);
    tuple->items[6] = mp_obj_new_int_from_uint(stat_buffer.st_size);
    tuple->items[7] = mp_obj_new_int_from_uint(stat_buffer.st_atime);
    tuple->items[8] = mp_obj_new_int_from_uint(stat_buffer.st_mtime);
    tuple->items[9] = mp_obj_new_int_from_uint(stat_buffer.st_ctime);
    return MP_OBJ_FROM_PTR(tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_2(esp_vision_storage_vfs_stat_obj, esp_vision_storage_vfs_stat);

static mp_obj_t esp_vision_storage_vfs_statvfs(mp_obj_t self_in, mp_obj_t path_in)
{
    esp_vision_storage_vfs_obj_t *self = MP_OBJ_TO_PTR(self_in);
    (void)path_in;

    uint64_t total_bytes;
    uint64_t free_bytes;
    esp_err_t ret = esp_vfs_fat_info(self->base_path, &total_bytes, &free_bytes);
    if (ret != ESP_OK) {
        mp_raise_OSError(MP_EIO);
    }

    mp_obj_tuple_t *tuple = MP_OBJ_TO_PTR(mp_obj_new_tuple(10, NULL));
    tuple->items[0] = MP_OBJ_NEW_SMALL_INT(ESP_VISION_STORAGE_BLOCK_SIZE);
    tuple->items[1] = MP_OBJ_NEW_SMALL_INT(ESP_VISION_STORAGE_BLOCK_SIZE);
    tuple->items[2] = mp_obj_new_int_from_ull(total_bytes / ESP_VISION_STORAGE_BLOCK_SIZE);
    tuple->items[3] = mp_obj_new_int_from_ull(free_bytes / ESP_VISION_STORAGE_BLOCK_SIZE);
    tuple->items[4] = tuple->items[3];
    tuple->items[5] = MP_OBJ_NEW_SMALL_INT(0);
    tuple->items[6] = MP_OBJ_NEW_SMALL_INT(0);
    tuple->items[7] = MP_OBJ_NEW_SMALL_INT(0);
    tuple->items[8] = MP_OBJ_NEW_SMALL_INT(self->readonly ? 1 : 0);
    tuple->items[9] = MP_OBJ_NEW_SMALL_INT(255);
    return MP_OBJ_FROM_PTR(tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_2(esp_vision_storage_vfs_statvfs_obj, esp_vision_storage_vfs_statvfs);

static mp_import_stat_t esp_vision_storage_vfs_import_stat(void *self_in, const char *path)
{
    esp_vision_storage_vfs_obj_t *self = self_in;
    char internal_path[MICROPY_ALLOC_PATH_MAX + 1];
    char full_path[ESP_VISION_STORAGE_FULL_PATH_MAX];
    esp_vision_storage_make_path_raw(self, path, strlen(path), internal_path, full_path);

    struct stat stat_buffer;
    if (stat(full_path, &stat_buffer) != 0) {
        return MP_IMPORT_STAT_NO_EXIST;
    }
    if (S_ISDIR(stat_buffer.st_mode)) {
        return MP_IMPORT_STAT_DIR;
    }
    if (S_ISREG(stat_buffer.st_mode)) {
        return MP_IMPORT_STAT_FILE;
    }
    return MP_IMPORT_STAT_NO_EXIST;
}

static const mp_rom_map_elem_t esp_vision_storage_vfs_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_mount), MP_ROM_PTR(&esp_vision_storage_vfs_mount_obj)},
    {MP_ROM_QSTR(MP_QSTR_umount), MP_ROM_PTR(&esp_vision_storage_vfs_umount_obj)},
    {MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&esp_vision_storage_vfs_open_obj)},
    {MP_ROM_QSTR(MP_QSTR_chdir), MP_ROM_PTR(&esp_vision_storage_vfs_chdir_obj)},
    {MP_ROM_QSTR(MP_QSTR_getcwd), MP_ROM_PTR(&esp_vision_storage_vfs_getcwd_obj)},
    {MP_ROM_QSTR(MP_QSTR_ilistdir), MP_ROM_PTR(&esp_vision_storage_vfs_ilistdir_obj)},
    {MP_ROM_QSTR(MP_QSTR_mkdir), MP_ROM_PTR(&esp_vision_storage_vfs_mkdir_obj)},
    {MP_ROM_QSTR(MP_QSTR_remove), MP_ROM_PTR(&esp_vision_storage_vfs_remove_obj)},
    {MP_ROM_QSTR(MP_QSTR_rename), MP_ROM_PTR(&esp_vision_storage_vfs_rename_obj)},
    {MP_ROM_QSTR(MP_QSTR_rmdir), MP_ROM_PTR(&esp_vision_storage_vfs_rmdir_obj)},
    {MP_ROM_QSTR(MP_QSTR_stat), MP_ROM_PTR(&esp_vision_storage_vfs_stat_obj)},
    {MP_ROM_QSTR(MP_QSTR_statvfs), MP_ROM_PTR(&esp_vision_storage_vfs_statvfs_obj)},
};
static MP_DEFINE_CONST_DICT(esp_vision_storage_vfs_locals, esp_vision_storage_vfs_locals_table);

static const mp_vfs_proto_t esp_vision_storage_vfs_protocol = {
    .import_stat = esp_vision_storage_vfs_import_stat,
};

MP_DEFINE_CONST_OBJ_TYPE(
    esp_vision_storage_vfs_type,
    MP_QSTR_IDFVfs,
    MP_TYPE_FLAG_NONE,
    protocol, &esp_vision_storage_vfs_protocol,
    locals_dict, &esp_vision_storage_vfs_locals
);

static mp_obj_t esp_vision_storage_vfs_new(const char *base_path)
{
    esp_vision_storage_vfs_obj_t *self = mp_obj_malloc(esp_vision_storage_vfs_obj_t,
                                                       &esp_vision_storage_vfs_type);
    self->base_path = base_path;
    self->cwd[0] = '/';
    self->cwd[1] = '\0';
    self->readonly = false;
    return MP_OBJ_FROM_PTR(self);
}

static void esp_vision_storage_mount_volume(const char *base_path, const char *mount_path)
{
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t args[] = {
            esp_vision_storage_vfs_new(base_path),
            MP_OBJ_NEW_QSTR(qstr_from_str(mount_path)),
        };
        mp_vfs_mount(MP_ARRAY_SIZE(args), args, (mp_map_t *)&mp_const_empty_map);
        nlr_pop();
    } else {
        ESP_LOGE(TAG, "failed to publish MicroPython mount %s", mount_path);
        mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(nlr.ret_val));
    }
}

void esp_vision_storage_mount_micropython(void)
{
    if (esp_vision_storage_flash_is_mounted()) {
        esp_vision_storage_mount_volume(ESP_VISION_STORAGE_FLASH_PATH, "/");
    }

    /* Storage initialization already attempted the cold-boot SD mount. Retry
     * only on later VM starts so an absent card does not incur two probes at
     * cold boot, while a card inserted later is picked up on soft reset. */
    if (s_initial_vm_mount_done) {
        esp_vision_storage_sdcard_try_mount();
    } else {
        s_initial_vm_mount_done = true;
    }
    if (esp_vision_storage_sdcard_is_mounted()) {
        esp_vision_storage_mount_volume(ESP_VISION_STORAGE_SDCARD_PATH,
                                        ESP_VISION_STORAGE_SDCARD_PATH);
    }
}
