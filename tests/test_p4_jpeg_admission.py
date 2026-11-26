"""Run production P4 JPEG admission with independent arrival/presentation clocks."""

from pathlib import Path
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[1]


def method(source, signature):
    start = source.index(signature)
    return source[start : source.index("\n}", start) + 2]


@pytest.fixture(scope="module", params=[False, True], ids=["normal", "debug"])
def admission_binary(tmp_path_factory, request):
    folder = tmp_path_factory.mktemp("p4-jpeg-admission")
    source = (ROOT / "esphome/components/p4_video_renderer/p4_video_renderer.cpp").read_text()
    methods = "\n".join(method(source, signature) for signature in (
        "bool P4VideoRenderer::set_active(", "bool P4VideoRenderer::submit_frame(",
        "void P4VideoRenderer::stop()",
    ))
    code = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include "esphome/components/video_renderer/video_renderer.h"
using namespace esphome;
uint32_t clock_us=0;uint32_t micros(){return clock_us;}
unsigned copies=0,notifications=0;
void* tracked_copy(void*to,const void*from,size_t size){++copies;return std::memcpy(to,from,size);}
#define memcpy tracked_copy
void xTaskNotifyGive(void*){++notifications;}
constexpr int pdTRUE=1;int xSemaphoreTake(void*,int){return pdTRUE;}void xSemaphoreGive(void*){}
class P4VideoRenderer : public video_renderer::EncodedFrameRenderer {
 public:
  std::atomic<bool>rx_running_{true},rx_active_{false},rx_session_prepared_{true};
  std::atomic<bool>remote_frame_visible_{false},presentation_in_flight_{false},video_ended_pending_{false};
  std::atomic<int>pending_surface_{-1};std::atomic<uint8_t>rx_slot_state_{0};
  uint8_t storage[32]{};uint8_t*rx_au_=storage;size_t rx_au_capacity_=sizeof(storage),rx_au_size_=0;
  uint64_t rx_timestamp_us_=0;uint32_t rx_au_generation_=0;bool rx_key_frame_=false;
  video_renderer::JpegFrameInfo rx_au_jpeg_info_{};
  std::atomic<uint32_t>rx_session_generation_{1},rx_admitted_frames_{0};
  std::atomic<uint32_t>rx_rate_drops_{0},rx_busy_drops_{0},rx_busy_pending_drops_{0},rx_busy_au_drops_{0};
  video_renderer::FrameCadence cadence_;video_renderer::StreamFormat rx_format_{video_renderer::Codec::JPEG,320,240,5};
  uint8_t framerate_=10;void*rx_task_handle_=this;void*presentation_mutex_=this;
  video_renderer::RendererCapabilities get_capabilities()const override{return {{video_renderer::Codec::JPEG,320,240,framerate_},320,240,32,false};}
  bool prepare(const video_renderer::StreamFormat&f)override{rx_format_=f;rx_session_prepared_=true;return f.valid();}
  bool start()override{return true;}bool set_active(bool)override;void stop()override;
  bool submit_frame(const video_renderer::EncodedFrame&)override;
  void enable_loop_soon_any_context(){}void dump_state()const{}
  void consume_slot(){rx_slot_state_.store(0);}
};
@METHODS@
int main(int argc,char**argv){
 assert(argc==2);const std::string scenario=argv[1];P4VideoRenderer renderer;
 assert(renderer.set_active(true));uint8_t payload[4]{0xff,0xd8,0xff,0xd9};unsigned releases=0;
 video_renderer::EncodedFrame frame{};frame.data=payload;frame.size=sizeof(payload);frame.key_frame=true;
 frame.release_callback=[](void*ctx){++*static_cast<unsigned*>(ctx);};frame.release_ctx=&releases;
 if(scenario=="valid"||scenario=="missing"||scenario=="backwards"||scenario=="wrap"){
  unsigned accepted=0;const uint32_t origin=scenario=="wrap"?UINT32_MAX-12345:0;
  for(unsigned i=0;i<=20;++i){
   clock_us=origin+i*50000;frame.presentation_time={scenario=="backwards"?1000000ULL-i*50000ULL:i*50000ULL,scenario!="missing"};
   const unsigned before=copies;const bool ok=renderer.submit_frame(frame);accepted+=ok;
   assert(copies==before+(ok?1:0));
   if(ok)assert(renderer.rx_timestamp_us_==(frame.presentation_time.valid?frame.presentation_time.microseconds:0));
   renderer.consume_slot();
  }
  assert(accepted==6&&releases==6&&copies==6&&notifications==6);
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
  assert(renderer.rx_rate_drops_==15);
#endif
  std::cout<<"{\"offered\":21,\"accepted\":"<<accepted<<",\"copies\":"<<copies<<",\"releases\":"<<releases<<"}";return 0;
 }
 if(scenario=="burst"){
  for(unsigned i=0;i<30;++i){frame.presentation_time={i*1000000ULL,true};const bool ok=renderer.submit_frame(frame);assert(ok==(i==0));renderer.consume_slot();}
  assert(releases==1&&copies==1);clock_us=200000;assert(renderer.submit_frame(frame));assert(releases==2);return 0;
 }
 if(scenario=="busy-preserves-credit"){
  assert(renderer.submit_frame(frame));renderer.consume_slot();clock_us=200000;
  renderer.pending_surface_=0;assert(!renderer.submit_frame(frame));renderer.pending_surface_=-1;
  renderer.rx_slot_state_=2;assert(!renderer.submit_frame(frame));renderer.consume_slot();
  assert(renderer.submit_frame(frame));assert(copies==2&&releases==2);return 0;
 }
 if(scenario=="lease-rejections"){
  assert(renderer.submit_frame(frame));renderer.consume_slot();const auto copy_count=copies;
  frame.size=33;assert(!renderer.submit_frame(frame));frame.size=4;
  frame.data=nullptr;assert(!renderer.submit_frame(frame));frame.data=payload;
  assert(!renderer.submit_frame(frame));assert(copies==copy_count&&releases==1&&renderer.rx_slot_state_==0);
  assert(renderer.set_active(false));clock_us=1000000;assert(!renderer.submit_frame(frame));assert(releases==1);
  renderer.stop();assert(releases==1&&!renderer.set_active(true));return 0;
 }
 if(scenario=="restart"){
  assert(renderer.submit_frame(frame));renderer.consume_slot();assert(!renderer.submit_frame(frame));
  assert(renderer.set_active(false));renderer.prepare({video_renderer::Codec::JPEG,320,240,20});
  assert(renderer.set_active(true)&&renderer.submit_frame(frame));renderer.consume_slot();
  clock_us=50000;assert(!renderer.submit_frame(frame));clock_us=100000;assert(renderer.submit_frame(frame));
  assert(releases==3&&copies==3);return 0;
 }
 assert(false&&"unknown scenario");
}
'''.replace("@METHODS@", methods)
    cpp = folder / "admission.cpp"
    cpp.write_text(code)
    binary = folder / "admission"
    flags = ["-DUSE_P4_VIDEO_RENDERER_DEBUG"] if request.param else []
    compiled = subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
        "-DUSE_P4_VIDEO_RENDERER_JPEG", *flags, "-I", str(ROOT), str(cpp), "-o", str(binary),
    ], capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stderr
    return binary


@pytest.mark.parametrize("scenario", [
    "valid", "missing", "backwards", "wrap", "burst", "busy-preserves-credit",
    "lease-rejections", "restart",
])
def test_jpeg_work_is_capped_before_copy_without_taking_rejected_leases(admission_binary, scenario):
    result = subprocess.run([str(admission_binary), scenario], capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stdout + result.stderr
