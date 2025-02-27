//////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2004-2021 musikcube team
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//    * Redistributions of source code must retain the above copyright notice,
//      this list of conditions and the following disclaimer.
//
//    * Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//
//    * Neither the name of the author nor the names of other contributors may
//      be used to endorse or promote products derived from this software
//      without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <musikcore/sdk/constants.h>
#include <musikcore/sdk/IDecoder.h>
#include <musikcore/sdk/IDataStream.h>

extern "C" {
    #pragma warning(push, 0)
    #include <libavformat/avio.h>
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libavcodec/version.h>
    #include <libavutil/samplefmt.h>
    #include <libavutil/audio_fifo.h>
    #include <libswresample/swresample.h>
    #pragma warning(pop)
}

#include <stddef.h>

#if LIBAVCODEC_VERSION_MAJOR >= 59
using AVCodecCompat = const AVCodec;
#else
using AVCodecCompat = AVCodec;
#endif

using namespace musik::core::sdk;

class FfmpegDecoder: public musik::core::sdk::IDecoder {
    public:
        FfmpegDecoder();
        ~FfmpegDecoder();

        void Release() override;
        double SetPosition(double seconds) override;
        bool GetBuffer(IBuffer *buffer) override;
        double GetDuration() override;
        bool Open(musik::core::sdk::IDataStream *stream) override;
        bool Exhausted() override;
        void SetPreferredSampleRate(int rate) override { this->preferredSampleRate = rate; }

        IDataStream* Stream() { return this->stream; }

    private:
        void Reset();
        AVFrame* AllocFrame(AVFrame* original, AVSampleFormat format, int sampleRate, int frameSize = -1);
        bool RefillFifoQueue();
        bool DrainResamplerToFifoQueue();
        bool ReadFromFifoAndWriteToBuffer(IBuffer* buffer);
        bool InitializeResampler();
        bool ReadSendAndReceivePacket(AVPacket* packet);
        void FlushAndFinalizeDecoder();

        musik::core::sdk::IDataStream* stream;
        AVIOContext* ioContext;
        AVAudioFifo* outputFifo;
        AVFormatContext* formatContext;
        AVCodecContext* codecContext;
        AVFrame* decodedFrame;
        AVFrame* resampledFrame;
        SwrContext* resampler;
        unsigned char* buffer;
        int preferredSampleRate { -1 };
        bool disableInvalidPacketDetection { false };
        int bufferSize;
        int rate, channels;
        int streamId;
        int preferredFrameSize;
        double duration;
        bool exhausted{ false };
        bool eof{ false };
};
