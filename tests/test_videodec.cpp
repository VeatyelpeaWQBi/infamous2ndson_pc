// SPDX-License-Identifier: GPL-2.0-or-later
// Generate our own H.264 access unit and decode it through the production PS4 ABI.
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
#include "core/libraries/videodec/videodec.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}
int main() {
    using namespace Libraries::Videodec;
    const AVCodec* codec=avcodec_find_encoder_by_name("libx264");
    assert(codec); // part of the installed FFmpeg package; absence must fail
    AVCodecContext* encoder=avcodec_alloc_context3(codec); assert(encoder);
    encoder->width=16; encoder->height=16; encoder->pix_fmt=AV_PIX_FMT_YUV420P;
    encoder->time_base={1,30}; encoder->framerate={30,1}; encoder->max_b_frames=0;
    assert(av_opt_set(encoder->priv_data,"preset","ultrafast",0)==0);
    assert(av_opt_set(encoder->priv_data,"tune","zerolatency",0)==0);
    assert(av_opt_set(encoder->priv_data,"qp","0",0)==0);
    assert(avcodec_open2(encoder,codec,nullptr)==0);
    AVFrame* frame=av_frame_alloc(); assert(frame);
    frame->format=encoder->pix_fmt; frame->width=16; frame->height=16; frame->pts=123;
    assert(av_frame_get_buffer(frame,32)==0);
    for (int plane=0;plane<3;++plane) {
        const int height=plane ? 8 : 16, width=plane ? 8 : 16;
        for (int y=0;y<height;++y) std::memset(frame->data[plane]+y*frame->linesize[plane],plane==0 ? 80 : plane==1 ? 90 : 240,width);
    }
    assert(avcodec_send_frame(encoder,frame)==0);
    AVPacket* packet=av_packet_alloc(); assert(packet && avcodec_receive_packet(encoder,packet)==0);
    OrbisVideodecConfigInfo config{}; config.thisSize=sizeof(config);
    config.maxFrameWidth=16; config.maxFrameHeight=16; config.maxDpbFrameCount=2;
    OrbisVideodecResourceInfo resources{}; resources.thisSize=sizeof(resources);
    assert(sceVideodecQueryResourceInfo(nullptr,&resources)==s32(0x80c1000f));
    assert(sceVideodecQueryResourceInfo(&config,&resources)==0);
    assert(resources.maxFrameBufferSize==6144 && resources.frameBufferAlignment==256);
    OrbisVideodecCtrl control{};
    assert(sceVideodecCreateDecoder(&config,&resources,&control)==0 && control.handle);
    const auto stale=control;
    std::vector<u8> memory(resources.maxFrameBufferSize+32,0x55);
    OrbisVideodecFrameBuffer buffer{sizeof(buffer),memory.data()+16,resources.maxFrameBufferSize};
    OrbisVideodecPictureInfo picture{}; picture.thisSize=sizeof(picture);
    OrbisVideodecInputData input{sizeof(input),packet->data,u64(packet->size),123,123,99};
    assert(sceVideodecDecode(&control,&input,nullptr,&picture)==s32(0x80c1000f));
    auto bad_input=input; bad_input.auSize=u64(INT32_MAX)+1;
    assert(sceVideodecDecode(&control,&bad_input,&buffer,&picture)==s32(0x80c10009));
    assert(sceVideodecDecode(&control,&input,&buffer,&picture)==0 && picture.isValid);
    assert(picture.frameWidth==16 && picture.framePitch==64 && picture.frameHeight==16);
    assert(picture.ptsData==123 && picture.attachedData==99);
    const auto* pixels=static_cast<u8*>(buffer.pFrameBuffer);
    for (unsigned y=0;y<16;++y) for (unsigned x=0;x<16;++x) assert(pixels[y*64+x]==80);
    for (unsigned y=0;y<8;++y) for (unsigned x=0;x<16;x+=2) {
        assert(pixels[64*16+y*64+x]==90 && pixels[64*16+y*64+x+1]==240);
    }
    for (unsigned i=0;i<16;++i) assert(memory[i]==0x55 && memory[memory.size()-1-i]==0x55);
    assert(sceVideodecFlush(&control,&buffer,&picture)==0 && !picture.isValid);
    assert(sceVideodecReset(&control)==0);
    buffer.frameBufferSize=1;
    assert(sceVideodecDecode(&control,&input,&buffer,&picture)==s32(0x80c1000b) && !picture.isValid);
    buffer.frameBufferSize=resources.maxFrameBufferSize;
    assert(sceVideodecFlush(&control,&buffer,&picture)==0 && picture.isValid);
    assert(sceVideodecDeleteDecoder(&control)==0 && !control.handle);
    assert(sceVideodecDeleteDecoder(&control)==s32(0x80c10003));
    auto old=stale; assert(sceVideodecReset(&old)==s32(0x80c10003));
    av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&encoder);
    std::puts("Videodec: H.264 pixels/PTS/opaque, drain/reset, short-buffer retry and stale handles PASS");
}
