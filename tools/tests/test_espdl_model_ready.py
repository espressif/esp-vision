# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Fault-inject native DL construction while executing production binding guards."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_jpeg_validation import function

ROOT = Path(__file__).resolve().parents[2]

PRELUDE = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <setjmp.h>
#include <string>
#include <vector>
static size_t cpp_allocations,native_live,native_frees,models_constructed,models_destroyed,preprocessors;
void *operator new(size_t n){void *p=malloc(n);assert(p);cpp_allocations++;return p;}
void operator delete(void *p) noexcept{free(p);}
void operator delete(void *p,size_t) noexcept{free(p);}
static struct{void *p;bool live;} allocations[64];
static void *native_alloc(size_t n){for(auto &a:allocations)if(!a.live){a.p=malloc(n);assert(a.p);a.live=true;native_live++;return a.p;}abort();}
static void heap_caps_free(void *p){if(!p)return;for(auto &a:allocations)if(a.live&&a.p==p){a.live=false;free(p);native_live--;native_frees++;return;}abort();}
static uint8_t input_bytes[128],output_bytes[128];
static size_t address_probes;
static bool esp_ptr_byte_accessible(const void *p){
    address_probes++;uintptr_t a=reinterpret_cast<uintptr_t>(p);
    for(auto b:{reinterpret_cast<uintptr_t>(input_bytes),reinterpret_cast<uintptr_t>(output_bytes)})
        if(a>=b&&a<b+128)return true;
    return false;
}
static int next_mode;
namespace fbs{static const int MODEL_LOCATION_IN_FLASH_RODATA=0;}
namespace dl{
class TensorBase{
public:
    void *data;int bytes;std::vector<int> shape;
    TensorBase(void *p,int n):data(p),bytes(n){}
    virtual ~TensorBase()=default;
    int get_bytes(){return bytes;}
    virtual void *get_element_ptr(){return data;}
};
class ViewTensor:public TensorBase{
public:
    ViewTensor():TensorBase(nullptr,32){}
    void *get_element_ptr()override{return output_bytes;}
};
static TensorBase input(input_bytes,64),output(output_bytes,32),scalar(input_bytes,1);
static TensorBase null_data(nullptr,64),invalid(reinterpret_cast<void *>(0x40),32);
static TensorBase overflow(reinterpret_cast<void *>(UINTPTR_MAX-7),16),zero(input_bytes,0);
static ViewTensor view;
class Model{
    bool loaded;void *parameters,*arena;
    std::map<std::string,TensorBase *> inputs,outputs;
public:
    static void *operator new(size_t n,const std::nothrow_t &)noexcept{return next_mode==13?nullptr:malloc(n);}
    static void operator delete(void *p)noexcept{free(p);}
    Model(const char *,int):loaded(next_mode!=2),parameters(native_alloc(16)),arena(nullptr){
        models_constructed++;
        // Mirrors the external Model's observable state after root_alloc=false:
        // a loaded FBS object with null input/output tensor entries.
        inputs.emplace("input",next_mode==1?nullptr:&input);
        outputs.emplace("output",next_mode==1?nullptr:&output);
        if(next_mode!=1)arena=native_alloc(16);
        if(next_mode==3)inputs.begin()->second=&null_data;
        if(next_mode==4)outputs.begin()->second=nullptr;
        if(next_mode==5)inputs.clear();
        if(next_mode==6)outputs.clear();
        if(next_mode==7)inputs.begin()->second=&invalid;
        if(next_mode==8)inputs.begin()->second=&overflow;
        if(next_mode==9)outputs.begin()->second=&zero;
        if(next_mode==10)outputs.begin()->second=&view;
        if(next_mode==11)inputs.begin()->second=&view;
        if(next_mode==12){inputs.emplace("scalar",&scalar);output.shape={1,2,3,4,5};}
    }
    virtual ~Model(){models_destroyed++;heap_caps_free(parameters);heap_caps_free(arena);}
    virtual void *get_fbs_model(){return loaded?this:nullptr;}
    // Exact ESP-DL 3.3.13 API contract: virtual getters return map references.
    virtual std::map<std::string,TensorBase *> &get_inputs(){return inputs;}
    virtual std::map<std::string,TensorBase *> &get_outputs(){return outputs;}
    virtual TensorBase *get_input(){return inputs.empty()?nullptr:inputs.begin()->second;}
    void minimize(){}
};
class VirtualModel:public Model{
    std::map<std::string,TensorBase *> virtual_inputs,virtual_outputs;
public:
    TensorBase *selected=&input;
    VirtualModel():Model(nullptr,0){virtual_inputs.emplace("virtual",&input);virtual_outputs.emplace("virtual",&view);}
    std::map<std::string,TensorBase *> &get_inputs()override{return virtual_inputs;}
    std::map<std::string,TensorBase *> &get_outputs()override{return virtual_outputs;}
    TensorBase *get_input()override{return selected;}
};
namespace image{
class ImagePreprocessor{
public:
    ImagePreprocessor(){preprocessors++;}
    ~ImagePreprocessor(){preprocessors--;}
};
}
}
namespace esp_vision{class NanoTrack{};}
struct mp_obj_type_t{};
static const mp_obj_type_t mp_type_MemoryError,mp_type_OSError;
using mp_rom_error_text_t=const char *;
using mp_obj_t=int;
#define MP_ERROR_TEXT(s) s
static jmp_buf jump;
static const mp_obj_type_t *raised;
[[noreturn]]static void mp_raise_msg(const mp_obj_type_t *type,mp_rom_error_text_t){raised=type;longjmp(jump,1);}
typedef struct{uint8_t *data;size_t size;}espdl_model_data_t;
typedef struct{espdl_model_data_t model_data;dl::Model *model;dl::image::ImagePreprocessor *image_preprocessor;}espdl_model_obj_t;
typedef struct{espdl_model_data_t data[3];dl::Model *models[3];esp_vision::NanoTrack *tracker;}espdl_nanotrack_obj_t;
typedef struct{int callback;espdl_nanotrack_obj_t *self;}espdl_nanotrack_cleanup_t;
static espdl_model_data_t espdl_read_model_from_vfs(mp_obj_t){return {static_cast<uint8_t *>(native_alloc(64)),64};}
'''
CASES = r'''
int main(int argc,char **argv){
    assert(argc==2);
    if(!strcmp(argv[1],"arena_failure")){
        espdl_model_obj_t self={};self.model_data=espdl_read_model_from_vfs(0);next_mode=1;
        if(!setjmp(jump)){
            self.model=espdl_create_model(&self.model_data,true);
            self.image_preprocessor=new dl::image::ImagePreprocessor();abort();
        }
        assert(raised==&mp_type_MemoryError&&!self.model&&!self.model_data.data&&!self.model_data.size);
        assert(models_destroyed==1&&!native_live&&!preprocessors);
        espdl_model_destroy(&self);espdl_model_destroy(&self);assert(models_destroyed==1);
    }else if(!strcmp(argv[1],"bad_tensors")){
        for(int mode:{3,4,5,6,7,8,9}){
            next_mode=mode;espdl_model_data_t data=espdl_read_model_from_vfs(0);
            if(!setjmp(jump)){espdl_create_model(&data,false);abort();}
            assert(raised==&mp_type_MemoryError&&!data.data&&!data.size&&!native_live);
        }
        assert(models_destroyed==7);
    }else if(!strcmp(argv[1],"load_and_object_failure")){
        for(int mode:{2,13}){
            next_mode=mode;espdl_model_data_t data=espdl_read_model_from_vfs(0);
            if(!setjmp(jump)){espdl_create_model(&data,false);abort();}
            assert(raised==(mode==2?&mp_type_OSError:&mp_type_MemoryError));
            assert(!data.data&&!data.size&&!native_live);
        }
        assert(models_constructed==1&&models_destroyed==1);
    }else if(!strcmp(argv[1],"valid_multi_scalar_virtual")){
        for(int mode:{0,10,12}){
            next_mode=mode;auto data=espdl_read_model_from_vfs(0);
            dl::Model *model=espdl_create_model(&data,true);
            size_t before=cpp_allocations;
            assert(espdl_model_readiness(model,true)==espdl_model_ready_t::ready);
            assert(cpp_allocations==before); // No copying maps or memory-info containers.
            delete model;espdl_free_model_data(&data);assert(!native_live);
        }
        assert(models_destroyed==3);
    }else if(!strcmp(argv[1],"virtual_input_contract")){
        next_mode=11;auto data=espdl_read_model_from_vfs(0);
        dl::Model *model=espdl_create_model(&data,false);
        assert(espdl_model_readiness(model,false)==espdl_model_ready_t::ready);
        assert(espdl_model_readiness(model,true)==espdl_model_ready_t::tensors_unavailable);
        delete model;espdl_free_model_data(&data);assert(!native_live);
    }else if(!strcmp(argv[1],"virtual_model_api")){
        next_mode=1;auto *derived=new(std::nothrow) dl::VirtualModel();
        dl::Model *model=derived;size_t before=cpp_allocations;
        assert(espdl_model_readiness(model,true)==espdl_model_ready_t::ready);
        assert(cpp_allocations==before);derived->selected=nullptr;
        assert(espdl_model_readiness(model,false)==espdl_model_ready_t::ready);
        assert(espdl_model_readiness(model,true)==espdl_model_ready_t::tensors_unavailable);
        delete model;assert(!native_live);
    }else if(!strcmp(argv[1],"span_overflow")){
        size_t before=address_probes;
        assert(!espdl_tensor_data_ready(&dl::overflow,false)&&address_probes==before);
        assert(!espdl_tensor_data_ready(nullptr,false)&&address_probes==before);
        assert(!espdl_tensor_data_ready(&dl::invalid,false));
        assert(espdl_tensor_data_ready(&dl::scalar,false));
    }else if(!strcmp(argv[1],"nanotrack_partial_failure")){
        espdl_nanotrack_obj_t self={};next_mode=0;espdl_nanotrack_load(&self,0,0);next_mode=1;
        if(!setjmp(jump)){espdl_nanotrack_load(&self,1,0);abort();}
        assert(raised==&mp_type_MemoryError&&!self.models[1]&&!self.data[1].data);
        espdl_nanotrack_destroy(&self);espdl_nanotrack_destroy(&self);
        assert(models_destroyed==2&&!native_live);
    }else if(!strcmp(argv[1],"nanotrack_lazy_failure")){
        espdl_nanotrack_obj_t self={};next_mode=0;
        espdl_nanotrack_load(&self,0,0);espdl_nanotrack_load(&self,1,0);next_mode=1;
        if(!setjmp(jump)){espdl_nanotrack_load(&self,2,0);abort();}
        espdl_nanotrack_cleanup_t cleanup={0,&self};espdl_nanotrack_release_template(&cleanup);
        assert(self.models[0]&&self.models[1]&&!self.models[2]&&native_live==6&&models_destroyed==1);
        espdl_nanotrack_destroy(&self);assert(models_destroyed==3&&!native_live);
    }else{abort();}
    assert(models_constructed==models_destroyed&&!native_live);
    return 0;
}
'''


class EspdlModelReadyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('c++') or shutil.which('g++')
        if not compiler:
            raise RuntimeError('C++ compiler required for ESP-DL production guard tests')
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        source = (ROOT / 'modules/py_espdl.cpp').read_text()
        helpers = source[source.index('enum class espdl_model_ready_t'):source.index('static dl::mem_info_t')]
        production = function(source, 'static void espdl_free_model_data(') + helpers
        for signature in ('static void espdl_model_destroy(', 'static void espdl_nanotrack_release_slot(',
                          'static void espdl_nanotrack_destroy(', 'static void espdl_nanotrack_release_template(',
                          'static void espdl_nanotrack_load('):
            production += '\n' + function(source, signature)
        directory = Path(cls.directory.name)
        unit = directory / 'ready.cpp'
        unit.write_text(PRELUDE + production + CASES)
        cls.executable = directory / 'ready'
        result = subprocess.run([compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                                 '-Wno-clobbered', '-fno-exceptions', '-fno-rtti', str(unit),
                                 '-o', str(cls.executable)], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_arena_oom_cleans_model_and_raw_data_before_python_memory_error(self):
        self.run_case('arena_failure')

    def test_null_maps_tensors_data_invalid_addresses_and_empty_bytes(self):
        self.run_case('bad_tensors')

    def test_model_object_and_fbs_load_failure_preserve_exception_type(self):
        self.run_case('load_and_object_failure')

    def test_valid_multi_input_scalar_arbitrary_rank_and_virtual_output(self):
        self.run_case('valid_multi_scalar_virtual')

    def test_virtual_input_allowed_for_general_models_but_raw_required_for_image(self):
        self.run_case('virtual_input_contract')

    def test_virtual_model_maps_and_selected_input_use_public_api(self):
        self.run_case('virtual_model_api')

    def test_pointer_span_overflow_and_null_offset_are_rejected(self):
        self.run_case('span_overflow')

    def test_nanotrack_partial_model_load_cleans_every_slot_once(self):
        self.run_case('nanotrack_partial_failure')

    def test_nanotrack_template_failure_preserves_search_and_head_until_cleanup(self):
        self.run_case('nanotrack_lazy_failure')

    def test_all_model_construction_routes_through_the_production_guard(self):
        source = (ROOT / 'modules/py_espdl.cpp').read_text()
        self.assertEqual(source.count('new (std::nothrow) dl::Model('), 1)
        for name in ('espdl_load_model', 'espdl_model_make_new', 'espdl_espdet_make_new',
                     'espdl_pedestrian_detect_make_new', 'espdl_yolo11_make_new',
                     'espdl_yolo11n_pose_make_new', 'espdl_imagenet_cls_make_new'):
            body = function(source, 'static mp_obj_t ' + name + '(')
            self.assertIn('espdl_create_model(', body)
            if '->minimize()' in body:
                self.assertLess(body.index('espdl_create_model('), body.index('->minimize()'))
        self.assertIn('espdl_create_model(', function(source, 'static void espdl_nanotrack_load('))


if __name__ == '__main__':
    unittest.main()
