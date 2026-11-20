#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <wels/codec_api.h>

#include "image.h"

// H.264 decoder used to independently check what the encoder really produced.
// It is a separate OpenH264 instance (software), so it cannot borrow state from
// either encoder backend.
class H264Decoder {
 public:
  ~H264Decoder() { close(); }

  bool open(std::string& err) {
    close();
    if (WelsCreateDecoder(&dec_) != 0 || !dec_) {
      err = "WelsCreateDecoder failed";
      return false;
    }
    SDecodingParam p;
    memset(&p, 0, sizeof p);
    p.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_DEFAULT;
    p.eEcActiveIdc = ERROR_CON_SLICE_COPY;
    p.uiTargetDqLayer = (uint8_t)-1;
    if (dec_->Initialize(&p) != 0) {
      err = "decoder Initialize failed";
      close();
      return false;
    }
    int quiet = WELS_LOG_QUIET;
    dec_->SetOption(DECODER_OPTION_TRACE_LEVEL, &quiet);
    return true;
  }

  void close() {
    if (dec_) {
      dec_->Uninitialize();
      WelsDestroyDecoder(dec_);
      dec_ = nullptr;
    }
  }

  // Decodes one access unit. Returns true when a picture came out.
  bool decode(const std::vector<uint8_t>& au, YuvView& out, std::string& err) {
    if (!dec_) {
      err = "decoder not open";
      return false;
    }
    unsigned char* dst[3] = {nullptr, nullptr, nullptr};
    SBufferInfo info;
    memset(&info, 0, sizeof info);
    DECODING_STATE st =
        dec_->DecodeFrameNoDelay(au.data(), (int)au.size(), dst, &info);
    if (st != dsErrorFree && st != dsFramePending) {
      // Keep going: error concealment may still have produced a picture, but
      // tell the caller the decode was not clean.
      err = "decode state " + std::to_string((int)st);
    }
    if (info.iBufferStatus != 1) return false;
    out.plane[0] = dst[0];
    out.plane[1] = dst[1];
    out.plane[2] = dst[2];
    out.stride[0] = info.UsrData.sSystemBuffer.iStride[0];
    out.stride[1] = info.UsrData.sSystemBuffer.iStride[1];
    out.stride[2] = info.UsrData.sSystemBuffer.iStride[1];
    out.width = info.UsrData.sSystemBuffer.iWidth;
    out.height = info.UsrData.sSystemBuffer.iHeight;
    out.fmt = PixFmt::I420;
    return true;
  }

 private:
  ISVCDecoder* dec_ = nullptr;
};
