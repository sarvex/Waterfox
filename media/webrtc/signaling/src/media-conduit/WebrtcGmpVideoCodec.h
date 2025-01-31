/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

// Class templates copied from WebRTC:
/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */


#ifndef WEBRTCGMPVIDEOCODEC_H_
#define WEBRTCGMPVIDEOCODEC_H_

#include <iostream>
#include <queue>
#include <string>

#include "nsThreadUtils.h"
#include "mozilla/Monitor.h"
#include "mozilla/Mutex.h"

#include "mozIGeckoMediaPluginService.h"
#include "MediaConduitInterface.h"
#include "AudioConduit.h"
#include "VideoConduit.h"
#include "webrtc/modules/video_coding/codecs/interface/video_codec_interface.h"

#include "gmp-video-host.h"
#include "GMPVideoDecoderProxy.h"
#include "GMPVideoEncoderProxy.h"

#include "PeerConnectionImpl.h"

namespace mozilla {

// Class that allows code on the other side of webrtc.org to tell
// WebrtcGmpVideoEncoder/Decoder what PC they should send errors to.
// This is necessary because webrtc.org gives us no way to plumb the handle
// through, nor does it give us any way to inform it of an error that will
// make it back to the PC that cares (except for errors encountered
// synchronously in functions like InitEncode/Decode, which will not happen
// because GMP init is async).
// Right now, this is used in MediaPipelineFactory.
class WebrtcGmpPCHandleSetter
{
  public:
    explicit WebrtcGmpPCHandleSetter(const std::string& aPCHandle);

    ~WebrtcGmpPCHandleSetter();

    static std::string GetCurrentHandle();

  private:
    static std::string sCurrentHandle;
};

class GmpInitDoneRunnable : public nsRunnable
{
  public:
    explicit GmpInitDoneRunnable(const std::string& aPCHandle) :
      mResult(WEBRTC_VIDEO_CODEC_OK),
      mPCHandle(aPCHandle)
    {
    }

    NS_IMETHOD Run()
    {
      if (mResult == WEBRTC_VIDEO_CODEC_OK) {
        // Might be useful to notify the PeerConnection about successful init
        // someday.
        return NS_OK;
      }

      PeerConnectionWrapper wrapper(mPCHandle);
      if (wrapper.impl()) {
        wrapper.impl()->OnMediaError(mError);
      }
      return NS_OK;
    }

    void Dispatch(int32_t aResult, const std::string& aError = "")
    {
      mResult = aResult;
      mError = aError;
      nsCOMPtr<nsIThread> mainThread(do_GetMainThread());
      if (mainThread) {
        // For some reason, the compiler on CI is treating |this| as a const
        // pointer, despite the fact that we're in a non-const function. And,
        // interestingly enough, correcting this doesn't require a const_cast.
        mainThread->Dispatch(this, NS_DISPATCH_NORMAL);
      }
    }

    int32_t Result()
    {
      return mResult;
    }

  private:
    int32_t mResult;
    std::string mPCHandle;
    std::string mError;
};

class WebrtcGmpVideoEncoder : public GMPVideoEncoderCallbackProxy
{
public:
  WebrtcGmpVideoEncoder();
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(WebrtcGmpVideoEncoder);

  // Implement VideoEncoder interface, sort of.
  // (We cannot use |Release|, since that's needed for nsRefPtr)
  virtual const uint64_t PluginID()
  {
    return mCachedPluginId;
  }

  virtual int32_t InitEncode(const webrtc::VideoCodec* aCodecSettings,
                             int32_t aNumberOfCores,
                             uint32_t aMaxPayloadSize);

  virtual int32_t Encode(const webrtc::I420VideoFrame& aInputImage,
                         const webrtc::CodecSpecificInfo* aCodecSpecificInfo,
                         const std::vector<webrtc::VideoFrameType>* aFrameTypes);

  virtual int32_t RegisterEncodeCompleteCallback(
    webrtc::EncodedImageCallback* aCallback);

  virtual int32_t ReleaseGmp();

  virtual int32_t SetChannelParameters(uint32_t aPacketLoss,
                                       int aRTT);

  virtual int32_t SetRates(uint32_t aNewBitRate,
                           uint32_t aFrameRate);

  // GMPVideoEncoderCallback virtual functions.
  virtual void Terminated() override;

  virtual void Encoded(GMPVideoEncodedFrame* aEncodedFrame,
                       const nsTArray<uint8_t>& aCodecSpecificInfo) override;

  virtual void Error(GMPErr aError) override {
  }

private:
  virtual ~WebrtcGmpVideoEncoder();

  static void InitEncode_g(const nsRefPtr<WebrtcGmpVideoEncoder>& aThis,
                           const GMPVideoCodec& aCodecParams,
                           int32_t aNumberOfCores,
                           uint32_t aMaxPayloadSize,
                           const nsRefPtr<GmpInitDoneRunnable>& aInitDone);
  int32_t GmpInitDone(GMPVideoEncoderProxy* aGMP, GMPVideoHost* aHost,
                      const GMPVideoCodec& aCodecParams,
                      uint32_t aMaxPayloadSize,
                      std::string* aErrorOut);
  int32_t GmpInitDone(GMPVideoEncoderProxy* aGMP,
                      GMPVideoHost* aHost,
                      std::string* aErrorOut);
  int32_t InitEncoderForSize(unsigned short aWidth,
                             unsigned short aHeight,
                             std::string* aErrorOut);
  static void ReleaseGmp_g(nsRefPtr<WebrtcGmpVideoEncoder>& aEncoder);
  void Close_g();

  class InitDoneCallback : public GetGMPVideoEncoderCallback
  {
  public:
    InitDoneCallback(const nsRefPtr<WebrtcGmpVideoEncoder>& aEncoder,
                     const nsRefPtr<GmpInitDoneRunnable>& aInitDone,
                     const GMPVideoCodec& aCodecParams,
                     uint32_t aMaxPayloadSize)
      : mEncoder(aEncoder),
        mInitDone(aInitDone),
        mCodecParams(aCodecParams),
        mMaxPayloadSize(aMaxPayloadSize)
    {
    }

    virtual void Done(GMPVideoEncoderProxy* aGMP, GMPVideoHost* aHost) override
    {
      std::string errorOut;
      int32_t result = mEncoder->GmpInitDone(aGMP,
                                             aHost,
                                             mCodecParams,
                                             mMaxPayloadSize,
                                             &errorOut);

      mInitDone->Dispatch(result, errorOut);
    }

  private:
    nsRefPtr<WebrtcGmpVideoEncoder> mEncoder;
    nsRefPtr<GmpInitDoneRunnable> mInitDone;
    GMPVideoCodec mCodecParams;
    uint32_t mMaxPayloadSize;
  };

  int32_t Encode_g(const webrtc::I420VideoFrame* aInputImage,
                   const webrtc::CodecSpecificInfo* aCodecSpecificInfo,
                   const std::vector<webrtc::VideoFrameType>* aFrameTypes);
  void RegetEncoderForResolutionChange(
      uint32_t aWidth,
      uint32_t aHeight,
      const nsRefPtr<GmpInitDoneRunnable>& aInitDone);

  class InitDoneForResolutionChangeCallback : public GetGMPVideoEncoderCallback
  {
  public:
    InitDoneForResolutionChangeCallback(
        const nsRefPtr<WebrtcGmpVideoEncoder>& aEncoder,
        const nsRefPtr<GmpInitDoneRunnable>& aInitDone,
        uint32_t aWidth,
        uint32_t aHeight)
      : mEncoder(aEncoder),
        mInitDone(aInitDone),
        mWidth(aWidth),
        mHeight(aHeight)
    {
    }

    virtual void Done(GMPVideoEncoderProxy* aGMP, GMPVideoHost* aHost) override
    {
      std::string errorOut;
      int32_t result = mEncoder->GmpInitDone(aGMP, aHost, &errorOut);
      if (result != WEBRTC_VIDEO_CODEC_OK) {
        mInitDone->Dispatch(result, errorOut);
        return;
      }

      result = mEncoder->InitEncoderForSize(mWidth, mHeight, &errorOut);
      mInitDone->Dispatch(result, errorOut);
    }

  private:
    nsRefPtr<WebrtcGmpVideoEncoder> mEncoder;
    nsRefPtr<GmpInitDoneRunnable> mInitDone;
    uint32_t mWidth;
    uint32_t mHeight;
  };

  static int32_t SetRates_g(nsRefPtr<WebrtcGmpVideoEncoder> aThis,
                             uint32_t aNewBitRate,
                             uint32_t aFrameRate);

  nsCOMPtr<mozIGeckoMediaPluginService> mMPS;
  nsCOMPtr<nsIThread> mGMPThread;
  GMPVideoEncoderProxy* mGMP;
  // Used to handle a race where Release() is called while init is in progress
  bool mInitting;
  GMPVideoHost* mHost;
  GMPVideoCodec mCodecParams;
  uint32_t mMaxPayloadSize;
  // Protects mCallback
  Mutex mCallbackMutex;
  webrtc::EncodedImageCallback* mCallback;
  uint64_t mCachedPluginId;
  std::string mPCHandle;
};


// Basically a strong ref to a WebrtcGmpVideoEncoder, that also translates
// from Release() to WebrtcGmpVideoEncoder::ReleaseGmp(), since we need
// WebrtcGmpVideoEncoder::Release() for managing the refcount.
// The webrtc.org code gets one of these, so it doesn't unilaterally delete
// the "real" encoder.
class WebrtcVideoEncoderProxy : public WebrtcVideoEncoder
{
  public:
    WebrtcVideoEncoderProxy() :
      mEncoderImpl(new WebrtcGmpVideoEncoder)
    {}

    virtual ~WebrtcVideoEncoderProxy()
    {
      RegisterEncodeCompleteCallback(nullptr);
    }

    const uint64_t PluginID() override
    {
      return mEncoderImpl->PluginID();
    }

    int32_t InitEncode(const webrtc::VideoCodec* aCodecSettings,
                       int32_t aNumberOfCores,
                       uint32_t aMaxPayloadSize) override
    {
      return mEncoderImpl->InitEncode(aCodecSettings,
                                      aNumberOfCores,
                                      aMaxPayloadSize);
    }

    int32_t Encode(
        const webrtc::I420VideoFrame& aInputImage,
        const webrtc::CodecSpecificInfo* aCodecSpecificInfo,
        const std::vector<webrtc::VideoFrameType>* aFrameTypes) override
    {
      return mEncoderImpl->Encode(aInputImage,
                                  aCodecSpecificInfo,
                                  aFrameTypes);
    }

    int32_t RegisterEncodeCompleteCallback(
      webrtc::EncodedImageCallback* aCallback) override
    {
      return mEncoderImpl->RegisterEncodeCompleteCallback(aCallback);
    }

    int32_t Release() override
    {
      return mEncoderImpl->ReleaseGmp();
    }

    int32_t SetChannelParameters(uint32_t aPacketLoss,
                                 int aRTT) override
    {
      return mEncoderImpl->SetChannelParameters(aPacketLoss, aRTT);
    }

    int32_t SetRates(uint32_t aNewBitRate,
                     uint32_t aFrameRate) override
    {
      return mEncoderImpl->SetRates(aNewBitRate, aFrameRate);
    }

  private:
    nsRefPtr<WebrtcGmpVideoEncoder> mEncoderImpl;
};

class WebrtcGmpVideoDecoder : public GMPVideoDecoderCallbackProxy
{
public:
  WebrtcGmpVideoDecoder();
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(WebrtcGmpVideoDecoder);

  // Implement VideoEncoder interface, sort of.
  // (We cannot use |Release|, since that's needed for nsRefPtr)
  virtual const uint64_t PluginID()
  {
    return mCachedPluginId;
  }

  virtual int32_t InitDecode(const webrtc::VideoCodec* aCodecSettings,
                             int32_t aNumberOfCores);
  virtual int32_t Decode(const webrtc::EncodedImage& aInputImage,
                         bool aMissingFrames,
                         const webrtc::RTPFragmentationHeader* aFragmentation,
                         const webrtc::CodecSpecificInfo* aCodecSpecificInfo,
                         int64_t aRenderTimeMs);
  virtual int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* aCallback);

  virtual int32_t ReleaseGmp();

  virtual int32_t Reset();

  // GMPVideoDecoderCallbackProxy
  virtual void Terminated() override;

  virtual void Decoded(GMPVideoi420Frame* aDecodedFrame) override;

  virtual void ReceivedDecodedReferenceFrame(const uint64_t aPictureId) override {
    MOZ_CRASH();
  }

  virtual void ReceivedDecodedFrame(const uint64_t aPictureId) override {
    MOZ_CRASH();
  }

  virtual void InputDataExhausted() override {
  }

  virtual void DrainComplete() override {
  }

  virtual void ResetComplete() override {
  }

  virtual void Error(GMPErr aError) override {
     mDecoderStatus = aError;
  }

private:
  virtual ~WebrtcGmpVideoDecoder();

  static void InitDecode_g(
      const nsRefPtr<WebrtcGmpVideoDecoder>& aThis,
      const webrtc::VideoCodec* aCodecSettings,
      int32_t aNumberOfCores,
      const nsRefPtr<GmpInitDoneRunnable>& aInitDone);
  int32_t GmpInitDone(GMPVideoDecoderProxy* aGMP,
                      GMPVideoHost* aHost,
                      std::string* aErrorOut);
  static void ReleaseGmp_g(nsRefPtr<WebrtcGmpVideoDecoder>& aDecoder);
  void Close_g();

  class InitDoneCallback : public GetGMPVideoDecoderCallback
  {
  public:
    explicit InitDoneCallback(WebrtcGmpVideoDecoder* aDecoder,
                              const nsRefPtr<GmpInitDoneRunnable>& aInitDone)
      : mDecoder(aDecoder),
        mInitDone(aInitDone)
    {
    }

    virtual void Done(GMPVideoDecoderProxy* aGMP, GMPVideoHost* aHost)
    {
      std::string errorOut;
      int32_t result = mDecoder->GmpInitDone(aGMP, aHost, &errorOut);

      mInitDone->Dispatch(result, errorOut);
    }

  private:
    WebrtcGmpVideoDecoder* mDecoder;
    nsRefPtr<GmpInitDoneRunnable> mInitDone;
  };

  virtual int32_t Decode_g(const webrtc::EncodedImage& aInputImage,
                           bool aMissingFrames,
                           const webrtc::RTPFragmentationHeader* aFragmentation,
                           const webrtc::CodecSpecificInfo* aCodecSpecificInfo,
                           int64_t aRenderTimeMs);

  nsCOMPtr<mozIGeckoMediaPluginService> mMPS;
  nsCOMPtr<nsIThread> mGMPThread;
  GMPVideoDecoderProxy* mGMP; // Addref is held for us
  // Used to handle a race where Release() is called while init is in progress
  bool mInitting;
  GMPVideoHost* mHost;
  // Protects mCallback
  Mutex mCallbackMutex;
  webrtc::DecodedImageCallback* mCallback;
  Atomic<uint64_t> mCachedPluginId;
  GMPErr mDecoderStatus;
  std::string mPCHandle;
};

// Basically a strong ref to a WebrtcGmpVideoDecoder, that also translates
// from Release() to WebrtcGmpVideoDecoder::ReleaseGmp(), since we need
// WebrtcGmpVideoDecoder::Release() for managing the refcount.
// The webrtc.org code gets one of these, so it doesn't unilaterally delete
// the "real" encoder.
class WebrtcVideoDecoderProxy : public WebrtcVideoDecoder
{
  public:
    WebrtcVideoDecoderProxy() :
      mDecoderImpl(new WebrtcGmpVideoDecoder)
    {}

    virtual ~WebrtcVideoDecoderProxy()
    {
      RegisterDecodeCompleteCallback(nullptr);
    }

    const uint64_t PluginID() override
    {
      return mDecoderImpl->PluginID();
    }

    int32_t InitDecode(const webrtc::VideoCodec* aCodecSettings,
                       int32_t aNumberOfCores) override
    {
      return mDecoderImpl->InitDecode(aCodecSettings, aNumberOfCores);
    }

    int32_t Decode(
        const webrtc::EncodedImage& aInputImage,
        bool aMissingFrames,
        const webrtc::RTPFragmentationHeader* aFragmentation,
        const webrtc::CodecSpecificInfo* aCodecSpecificInfo,
        int64_t aRenderTimeMs) override
    {
      return mDecoderImpl->Decode(aInputImage,
                                  aMissingFrames,
                                  aFragmentation,
                                  aCodecSpecificInfo,
                                  aRenderTimeMs);
    }

    int32_t RegisterDecodeCompleteCallback(
      webrtc::DecodedImageCallback* aCallback) override
    {
      return mDecoderImpl->RegisterDecodeCompleteCallback(aCallback);
    }

    int32_t Release() override
    {
      return mDecoderImpl->ReleaseGmp();
    }

    int32_t Reset() override
    {
      return mDecoderImpl->Reset();
    }

  private:
    nsRefPtr<WebrtcGmpVideoDecoder> mDecoderImpl;
};

}

#endif
