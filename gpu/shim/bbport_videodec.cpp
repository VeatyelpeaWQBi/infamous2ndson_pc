// SPDX-License-Identifier: GPL-2.0-or-later
// Second Son direct Videodec HLE. ABI/resource sizing follows shadPS4 Videodec;
// Windows implementation owns FFmpeg state and validates handles and output bounds.
#include "core/libraries/videodec/videodec.h"
#include "core/libraries/libs.h"
#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
namespace Libraries::Videodec {
namespace {
constexpr s32 Fail=s32(0x80C10000), Codec=s32(0x80C10001), Size=s32(0x80C10002),
    Handle=s32(0x80C10003), AuSize=s32(0x80C10009), AuPointer=s32(0x80C1000A),
    BufferSize=s32(0x80C1000B), BufferPointer=s32(0x80C1000C),
    Config=s32(0x80C1000E), Pointer=s32(0x80C1000F);
static_assert(sizeof(OrbisVideodecConfigInfo)==40 && sizeof(OrbisVideodecResourceInfo)==56);
static_assert(sizeof(OrbisVideodecPictureInfo)==112 && sizeof(OrbisVideodecInputData)==48);
struct Decoder {
    AVCodecContext* codec{};
    SwsContext* converter{};
    AVFrame* pending{};
    bool draining{};
    ~Decoder() { av_frame_free(&pending); avcodec_free_context(&codec); sws_freeContext(converter); }
};
std::mutex mutex;
std::unordered_map<uintptr_t,std::unique_ptr<Decoder>> decoders;
uintptr_t next_handle=1;
s32 validate(const OrbisVideodecConfigInfo* config) {
    if (!config) return Pointer;
    if (config->thisSize!=sizeof(*config)) return Size;
    if (config->codecType!=0) return Codec; // AVC/H.264 only, used by Second Son
    if (config->maxFrameWidth<0 || config->maxFrameHeight<0 || config->maxFrameWidth>8192 ||
        config->maxFrameHeight>8192 || config->maxDpbFrameCount<0 || config->maxDpbFrameCount>32 ||
        config->videodecFlags) return Config;
    return 0;
}
Decoder* lookup(const OrbisVideodecCtrl* ctrl) {
    if (!ctrl || ctrl->thisSize!=sizeof(*ctrl) || ctrl->version!=1) return nullptr;
    const auto it=decoders.find(reinterpret_cast<uintptr_t>(ctrl->handle));
    return it==decoders.end() ? nullptr : it->second.get();
}
s32 frame_args(const OrbisVideodecCtrl* ctrl,const OrbisVideodecFrameBuffer* buffer,
               OrbisVideodecPictureInfo* picture) {
    if (!ctrl || !buffer || !picture) return Pointer;
    if (ctrl->thisSize!=sizeof(*ctrl) || buffer->thisSize!=sizeof(*buffer) || picture->thisSize!=sizeof(*picture)) return Size;
    if (!buffer->pFrameBuffer) return BufferPointer;
    return 0;
}
s32 receive(Decoder& decoder,OrbisVideodecFrameBuffer& buffer,OrbisVideodecPictureInfo& picture) {
    picture.isValid=0;
    if (!decoder.pending) {
        decoder.pending=av_frame_alloc();
        if (!decoder.pending) return Fail;
        const int result=avcodec_receive_frame(decoder.codec,decoder.pending);
        if (result<0) {
            av_frame_free(&decoder.pending);
            return result==AVERROR(EAGAIN) || result==AVERROR_EOF ? 0 : Fail;
        }
    }
    const AVFrame& frame=*decoder.pending;
    if (frame.width<=0 || frame.height<=0 || frame.width>8192 || frame.height>8192 ||
        frame.width%2 || frame.height%2) return Config;
    const u32 pitch=(frame.width+63u)&~63u, height=(frame.height+15u)&~15u;
    const u64 required=u64(pitch)*height*3/2;
    // Retain the decoded frame on a short output buffer so the caller can retry with Flush.
    if (buffer.frameBufferSize<required) return BufferSize;
    decoder.converter=sws_getCachedContext(decoder.converter,frame.width,frame.height,AVPixelFormat(frame.format),
        frame.width,frame.height,AV_PIX_FMT_NV12,SWS_FAST_BILINEAR,nullptr,nullptr,nullptr);
    if (!decoder.converter) return Fail;
    auto* dst=static_cast<u8*>(buffer.pFrameBuffer);
    std::memset(dst,0,required);
    u8* planes[4]={dst,dst+u64(pitch)*height,nullptr,nullptr};
    int strides[4]={int(pitch),int(pitch),0,0};
    if (sws_scale(decoder.converter,frame.data,frame.linesize,0,frame.height,planes,strides)!=frame.height) return Fail;
    for (u32 y=frame.height;y<height;++y) std::memcpy(dst+u64(y)*pitch,dst+u64(frame.height-1)*pitch,pitch);
    for (u32 y=frame.height/2;y<height/2;++y)
        std::memcpy(planes[1]+u64(y)*pitch,planes[1]+u64(frame.height/2-1)*pitch,pitch);
    const u64 struct_size=picture.thisSize;
    std::memset(&picture,0,sizeof(picture)); picture.thisSize=struct_size;
    picture.isValid=1; picture.frameWidth=(frame.width+15u)&~15u;
    picture.framePitch=pitch; picture.frameHeight=height; picture.ptsData=frame.pts;
    picture.attachedData=reinterpret_cast<u64>(frame.opaque);
    picture.codec.avc.colourPrimaries=u8(frame.color_primaries);
    picture.codec.avc.transferCharacteristics=u8(frame.color_trc);
    picture.codec.avc.matrixCoefficients=u8(frame.colorspace);
    picture.codec.avc.videoFullRangeFlag=frame.color_range==AVCOL_RANGE_JPEG;
    picture.codec.avc.frameCropRightOffset=pitch-frame.width;
    picture.codec.avc.frameCropBottomOffset=height-frame.height;
    av_frame_free(&decoder.pending);
    return 0;
}
} // namespace
s32 PS4_SYSV_ABI sceVideodecQueryResourceInfo(const OrbisVideodecConfigInfo* config,OrbisVideodecResourceInfo* resource) {
    if (!resource) return Pointer;
    if (const s32 error=validate(config)) return error;
    if (resource->thisSize!=sizeof(*resource)) return Size;
    const u32 width=config->maxFrameWidth>0 ? config->maxFrameWidth : (config->maxLevel>=150 ? 3840 : 1920);
    const u32 height=config->maxFrameHeight>0 ? config->maxFrameHeight : (config->maxLevel>=150 ? 2160 : 1080);
    const u64 frame=u64((width+255u)&~255u)*((height+15u)&~15u)*3/2;
    const u32 dpb=config->maxDpbFrameCount>0 ? config->maxDpbFrameCount : 8;
    resource->cpuMemorySize=(frame*(dpb+2)+4095)&~u64(4095);
    resource->cpuGpuMemorySize=(frame*(dpb+2)+4095)&~u64(4095);
    resource->maxFrameBufferSize=frame; resource->frameBufferAlignment=256;
    return 0;
}
s32 PS4_SYSV_ABI sceVideodecCreateDecoder(const OrbisVideodecConfigInfo* config,
    const OrbisVideodecResourceInfo* resource,OrbisVideodecCtrl* ctrl) {
    if (!resource || !ctrl) return Pointer;
    if (const s32 error=validate(config)) return error;
    if (resource->thisSize!=sizeof(*resource)) return Size;
    auto decoder=std::make_unique<Decoder>();
    const AVCodec* codec=avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec || !(decoder->codec=avcodec_alloc_context3(codec))) return Fail;
    decoder->codec->width=config->maxFrameWidth; decoder->codec->height=config->maxFrameHeight;
    decoder->codec->flags|=AV_CODEC_FLAG_COPY_OPAQUE;
    decoder->codec->thread_count=1;
    if (avcodec_open2(decoder->codec,codec,nullptr)<0) return Fail;
    std::scoped_lock lock{mutex};
    const auto id=next_handle++;
    decoders.emplace(id,std::move(decoder));
    *ctrl={sizeof(*ctrl),reinterpret_cast<void*>(id),1};
    return 0;
}
s32 PS4_SYSV_ABI sceVideodecDeleteDecoder(OrbisVideodecCtrl* ctrl) {
    if (!ctrl) return Pointer;
    if (ctrl->thisSize!=sizeof(*ctrl)) return Size;
    std::scoped_lock lock{mutex};
    if (!lookup(ctrl)) return Handle;
    decoders.erase(reinterpret_cast<uintptr_t>(ctrl->handle)); ctrl->handle=nullptr;
    return 0;
}
s32 PS4_SYSV_ABI sceVideodecDecode(OrbisVideodecCtrl* ctrl,const OrbisVideodecInputData* input,
    OrbisVideodecFrameBuffer* buffer,OrbisVideodecPictureInfo* picture) {
    if (!input) return Pointer;
    if (const s32 error=frame_args(ctrl,buffer,picture)) return error;
    if (input->thisSize!=sizeof(*input)) return Size;
    if (!input->pAuData) return AuPointer;
    if (!input->auSize || input->auSize>INT_MAX) return AuSize;
    std::scoped_lock lock{mutex};
    auto* decoder=lookup(ctrl); if (!decoder) return Handle;
    if (decoder->pending) return BufferSize; // consume the retained frame before submitting another AU
    if (decoder->draining) { avcodec_flush_buffers(decoder->codec); decoder->draining=false; }
    AVPacket* packet=av_packet_alloc(); if (!packet) return Fail;
    if (av_new_packet(packet,int(input->auSize))<0) { av_packet_free(&packet); return Fail; }
    std::memcpy(packet->data,input->pAuData,input->auSize); // FFmpeg requires readable zero padding
    packet->pts=input->ptsData; packet->dts=input->dtsData;
    packet->opaque=reinterpret_cast<void*>(input->attachedData);
    const int result=avcodec_send_packet(decoder->codec,packet);
    av_packet_free(&packet);
    if (result<0) return Fail;
    return receive(*decoder,*buffer,*picture);
}
s32 PS4_SYSV_ABI sceVideodecFlush(OrbisVideodecCtrl* ctrl,OrbisVideodecFrameBuffer* buffer,
    OrbisVideodecPictureInfo* picture) {
    if (const s32 error=frame_args(ctrl,buffer,picture)) return error;
    std::scoped_lock lock{mutex};
    auto* decoder=lookup(ctrl); if (!decoder) return Handle;
    if (!decoder->pending && !decoder->draining) {
        const int result=avcodec_send_packet(decoder->codec,nullptr);
        if (result<0 && result!=AVERROR_EOF) return Fail;
        decoder->draining=true;
    }
    return receive(*decoder,*buffer,*picture);
}
s32 PS4_SYSV_ABI sceVideodecReset(OrbisVideodecCtrl* ctrl) {
    if (!ctrl) return Pointer;
    if (ctrl->thisSize!=sizeof(*ctrl)) return Size;
    std::scoped_lock lock{mutex};
    auto* decoder=lookup(ctrl); if (!decoder) return Handle;
    avcodec_flush_buffers(decoder->codec); av_frame_free(&decoder->pending); decoder->draining=false;
    return 0;
}
void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("qkgRiwHyheU","libSceVideodec",1,"libSceVideodec",sceVideodecCreateDecoder);
    LIB_FUNCTION("q0W5GJMovMs","libSceVideodec",1,"libSceVideodec",sceVideodecDecode);
    LIB_FUNCTION("U0kpGF1cl90","libSceVideodec",1,"libSceVideodec",sceVideodecDeleteDecoder);
    LIB_FUNCTION("jeigLlKdp5I","libSceVideodec",1,"libSceVideodec",sceVideodecFlush);
    LIB_FUNCTION("leCAscipfFY","libSceVideodec",1,"libSceVideodec",sceVideodecQueryResourceInfo);
    LIB_FUNCTION("f8AgDv-1X8A","libSceVideodec",1,"libSceVideodec",sceVideodecReset);
}
} // namespace Libraries::Videodec
