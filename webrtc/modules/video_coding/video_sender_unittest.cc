/*
 *  Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <vector>

#include "testing/gtest/include/gtest/gtest.h"
#include "webrtc/base/scoped_ptr.h"
#include "webrtc/common.h"
#include "webrtc/modules/video_coding/include/mock/mock_video_codec_interface.h"
#include "webrtc/modules/video_coding/codecs/vp8/include/vp8.h"
#include "webrtc/modules/video_coding/codecs/vp8/include/vp8_common_types.h"
#include "webrtc/modules/video_coding/codecs/vp8/temporal_layers.h"
#include "webrtc/modules/video_coding/include/mock/mock_vcm_callbacks.h"
#include "webrtc/modules/video_coding/include/video_coding.h"
#include "webrtc/modules/video_coding/video_coding_impl.h"
#include "webrtc/modules/video_coding/test/test_util.h"
#include "webrtc/system_wrappers/include/clock.h"
#include "webrtc/test/frame_generator.h"
#include "webrtc/test/testsupport/fileutils.h"

using ::testing::_;
using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::Field;
using ::testing::NiceMock;
using ::testing::Pointee;
using ::testing::Return;
using ::testing::FloatEq;
using std::vector;
using webrtc::test::FrameGenerator;

namespace webrtc {
namespace vcm {
namespace {
enum { kMaxNumberOfTemporalLayers = 3 };

struct Vp8StreamInfo {
  float framerate_fps[kMaxNumberOfTemporalLayers];
  int bitrate_kbps[kMaxNumberOfTemporalLayers];
};

MATCHER_P(MatchesVp8StreamInfo, expected, "") {
  bool res = true;
  for (int tl = 0; tl < kMaxNumberOfTemporalLayers; ++tl) {
    if (fabs(expected.framerate_fps[tl] - arg.framerate_fps[tl]) > 0.5) {
      *result_listener << " framerate_fps[" << tl
                       << "] = " << arg.framerate_fps[tl] << " (expected "
                       << expected.framerate_fps[tl] << ") ";
      res = false;
    }
    if (abs(expected.bitrate_kbps[tl] - arg.bitrate_kbps[tl]) > 10) {
      *result_listener << " bitrate_kbps[" << tl
                       << "] = " << arg.bitrate_kbps[tl] << " (expected "
                       << expected.bitrate_kbps[tl] << ") ";
      res = false;
    }
  }
  return res;
}

class EmptyFrameGenerator : public FrameGenerator {
 public:
  EmptyFrameGenerator(int width, int height) : width_(width), height_(height) {}
  VideoFrame* NextFrame() override {
    frame_.reset(new VideoFrame());
    frame_->CreateEmptyFrame(width_, height_, width_, (width_ + 1) / 2,
                             (width_ + 1) / 2);
    return frame_.get();
  }

 private:
  const int width_;
  const int height_;
  rtc::scoped_ptr<VideoFrame> frame_;
};

class PacketizationCallback : public VCMPacketizationCallback {
 public:
  explicit PacketizationCallback(Clock* clock)
      : clock_(clock), start_time_ms_(clock_->TimeInMilliseconds()) {}

  virtual ~PacketizationCallback() {}

  int32_t SendData(uint8_t payload_type,
                   const EncodedImage& encoded_image,
                   const RTPFragmentationHeader& fragmentation_header,
                   const RTPVideoHeader* rtp_video_header) override {
    assert(rtp_video_header);
    frame_data_.push_back(FrameData(encoded_image._length, *rtp_video_header));
    return 0;
  }

  void Reset() {
    frame_data_.clear();
    start_time_ms_ = clock_->TimeInMilliseconds();
  }

  float FramerateFpsWithinTemporalLayer(int temporal_layer) {
    return CountFramesWithinTemporalLayer(temporal_layer) *
           (1000.0 / interval_ms());
  }

  float BitrateKbpsWithinTemporalLayer(int temporal_layer) {
    return SumPayloadBytesWithinTemporalLayer(temporal_layer) * 8.0 /
           interval_ms();
  }

  Vp8StreamInfo CalculateVp8StreamInfo() {
    Vp8StreamInfo info;
    for (int tl = 0; tl < 3; ++tl) {
      info.framerate_fps[tl] = FramerateFpsWithinTemporalLayer(tl);
      info.bitrate_kbps[tl] = BitrateKbpsWithinTemporalLayer(tl);
    }
    return info;
  }

 private:
  struct FrameData {
    FrameData() {}

    FrameData(size_t payload_size, const RTPVideoHeader& rtp_video_header)
        : payload_size(payload_size), rtp_video_header(rtp_video_header) {}

    size_t payload_size;
    RTPVideoHeader rtp_video_header;
  };

  int64_t interval_ms() {
    int64_t diff = (clock_->TimeInMilliseconds() - start_time_ms_);
    EXPECT_GT(diff, 0);
    return diff;
  }

  int CountFramesWithinTemporalLayer(int temporal_layer) {
    int frames = 0;
    for (size_t i = 0; i < frame_data_.size(); ++i) {
      EXPECT_EQ(kRtpVideoVp8, frame_data_[i].rtp_video_header.codec);
      const uint8_t temporal_idx =
          frame_data_[i].rtp_video_header.codecHeader.VP8.temporalIdx;
      if (temporal_idx <= temporal_layer || temporal_idx == kNoTemporalIdx)
        frames++;
    }
    return frames;
  }

  size_t SumPayloadBytesWithinTemporalLayer(int temporal_layer) {
    size_t payload_size = 0;
    for (size_t i = 0; i < frame_data_.size(); ++i) {
      EXPECT_EQ(kRtpVideoVp8, frame_data_[i].rtp_video_header.codec);
      const uint8_t temporal_idx =
          frame_data_[i].rtp_video_header.codecHeader.VP8.temporalIdx;
      if (temporal_idx <= temporal_layer || temporal_idx == kNoTemporalIdx)
        payload_size += frame_data_[i].payload_size;
    }
    return payload_size;
  }

  Clock* clock_;
  int64_t start_time_ms_;
  vector<FrameData> frame_data_;
};

class TestVideoSender : public ::testing::Test {
 protected:
  // Note: simulated clock starts at 1 seconds, since parts of webrtc use 0 as
  // a special case (e.g. frame rate in media optimization).
  TestVideoSender() : clock_(1000), packetization_callback_(&clock_) {}

  void SetUp() override {
    sender_.reset(
        new VideoSender(&clock_, &post_encode_callback_, nullptr, nullptr));
    EXPECT_EQ(0, sender_->RegisterTransportCallback(&packetization_callback_));
  }

  void AddFrame() {
    assert(generator_.get());
    sender_->AddVideoFrame(*generator_->NextFrame(), NULL, NULL);
  }

  SimulatedClock clock_;
  PacketizationCallback packetization_callback_;
  MockEncodedImageCallback post_encode_callback_;
  // Used by subclassing tests, need to outlive sender_.
  rtc::scoped_ptr<VideoEncoder> encoder_;
  rtc::scoped_ptr<VideoSender> sender_;
  rtc::scoped_ptr<FrameGenerator> generator_;
};

class TestVideoSenderWithMockEncoder : public TestVideoSender {
 protected:
  static const int kDefaultWidth = 1280;
  static const int kDefaultHeight = 720;
  static const int kNumberOfStreams = 3;
  static const int kNumberOfLayers = 3;
  static const int kUnusedPayloadType = 10;

  void SetUp() override {
    TestVideoSender::SetUp();
    sender_->RegisterExternalEncoder(&encoder_, kUnusedPayloadType, false);
    VideoCodingModule::Codec(kVideoCodecVP8, &settings_);
    settings_.numberOfSimulcastStreams = kNumberOfStreams;
    ConfigureStream(kDefaultWidth / 4, kDefaultHeight / 4, 100,
                    &settings_.simulcastStream[0]);
    ConfigureStream(kDefaultWidth / 2, kDefaultHeight / 2, 500,
                    &settings_.simulcastStream[1]);
    ConfigureStream(kDefaultWidth, kDefaultHeight, 1200,
                    &settings_.simulcastStream[2]);
    settings_.plType = kUnusedPayloadType;  // Use the mocked encoder.
    generator_.reset(
        new EmptyFrameGenerator(settings_.width, settings_.height));
    EXPECT_EQ(0, sender_->RegisterSendCodec(&settings_, 1, 1200));
  }

  void TearDown() override { sender_.reset(); }

  void ExpectIntraRequest(int stream) {
    if (stream == -1) {
      // No intra request expected.
      EXPECT_CALL(
          encoder_,
          Encode(_, _, Pointee(ElementsAre(kVideoFrameDelta, kVideoFrameDelta,
                                           kVideoFrameDelta))))
          .Times(1)
          .WillRepeatedly(Return(0));
      return;
    }
    assert(stream >= 0);
    assert(stream < kNumberOfStreams);
    std::vector<FrameType> frame_types(kNumberOfStreams, kVideoFrameDelta);
    frame_types[stream] = kVideoFrameKey;
    EXPECT_CALL(encoder_,
                Encode(_, _, Pointee(ElementsAreArray(&frame_types[0],
                                                      frame_types.size()))))
        .Times(1)
        .WillRepeatedly(Return(0));
  }

  static void ConfigureStream(int width,
                              int height,
                              int max_bitrate,
                              SimulcastStream* stream) {
    assert(stream);
    stream->width = width;
    stream->height = height;
    stream->maxBitrate = max_bitrate;
    stream->numberOfTemporalLayers = kNumberOfLayers;
    stream->qpMax = 45;
  }

  VideoCodec settings_;
  NiceMock<MockVideoEncoder> encoder_;
};

TEST_F(TestVideoSenderWithMockEncoder, TestIntraRequests) {
  EXPECT_EQ(0, sender_->IntraFrameRequest(0));
  ExpectIntraRequest(0);
  AddFrame();
  ExpectIntraRequest(-1);
  AddFrame();

  EXPECT_EQ(0, sender_->IntraFrameRequest(1));
  ExpectIntraRequest(1);
  AddFrame();
  ExpectIntraRequest(-1);
  AddFrame();

  EXPECT_EQ(0, sender_->IntraFrameRequest(2));
  ExpectIntraRequest(2);
  AddFrame();
  ExpectIntraRequest(-1);
  AddFrame();

  EXPECT_EQ(-1, sender_->IntraFrameRequest(3));
  ExpectIntraRequest(-1);
  AddFrame();

  EXPECT_EQ(-1, sender_->IntraFrameRequest(-1));
  ExpectIntraRequest(-1);
  AddFrame();
}

TEST_F(TestVideoSenderWithMockEncoder, TestIntraRequestsInternalCapture) {
  // De-register current external encoder.
  sender_->RegisterExternalEncoder(nullptr, kUnusedPayloadType, false);
  // Register encoder with internal capture.
  sender_->RegisterExternalEncoder(&encoder_, kUnusedPayloadType, true);
  EXPECT_EQ(0, sender_->RegisterSendCodec(&settings_, 1, 1200));
  ExpectIntraRequest(0);
  EXPECT_EQ(0, sender_->IntraFrameRequest(0));
  ExpectIntraRequest(1);
  EXPECT_EQ(0, sender_->IntraFrameRequest(1));
  ExpectIntraRequest(2);
  EXPECT_EQ(0, sender_->IntraFrameRequest(2));
  // No requests expected since these indices are out of bounds.
  EXPECT_EQ(-1, sender_->IntraFrameRequest(3));
  EXPECT_EQ(-1, sender_->IntraFrameRequest(-1));
}

TEST_F(TestVideoSenderWithMockEncoder, EncoderFramerateUpdatedViaProcess) {
  sender_->SetChannelParameters(settings_.startBitrate * 1000, 0, 200);
  const int64_t kRateStatsWindowMs = 2000;
  const uint32_t kInputFps = 20;
  int64_t start_time = clock_.TimeInMilliseconds();
  while (clock_.TimeInMilliseconds() < start_time + kRateStatsWindowMs) {
    AddFrame();
    clock_.AdvanceTimeMilliseconds(1000 / kInputFps);
  }
  EXPECT_CALL(encoder_, SetRates(_, kInputFps)).Times(1).WillOnce(Return(0));
  sender_->Process();
  AddFrame();
}

TEST_F(TestVideoSenderWithMockEncoder,
       NoRedundantSetChannelParameterOrSetRatesCalls) {
  const uint8_t kLossRate = 4;
  const uint8_t kRtt = 200;
  const int64_t kRateStatsWindowMs = 2000;
  const uint32_t kInputFps = 20;
  int64_t start_time = clock_.TimeInMilliseconds();
  // Expect initial call to SetChannelParameters. Rates are initialized through
  // InitEncode and expects no additional call before the framerate (or bitrate)
  // updates.
  EXPECT_CALL(encoder_, SetChannelParameters(kLossRate, kRtt))
      .Times(1)
      .WillOnce(Return(0));
  sender_->SetChannelParameters(settings_.startBitrate * 1000, kLossRate, kRtt);
  while (clock_.TimeInMilliseconds() < start_time + kRateStatsWindowMs) {
    AddFrame();
    clock_.AdvanceTimeMilliseconds(1000 / kInputFps);
  }
  // After process, input framerate should be updated but not ChannelParameters
  // as they are the same as before.
  EXPECT_CALL(encoder_, SetRates(_, kInputFps)).Times(1).WillOnce(Return(0));
  sender_->Process();
  AddFrame();
  // Call to SetChannelParameters with changed bitrate should call encoder
  // SetRates but not encoder SetChannelParameters (that are unchanged).
  EXPECT_CALL(encoder_, SetRates(2 * settings_.startBitrate, kInputFps))
      .Times(1)
      .WillOnce(Return(0));
  sender_->SetChannelParameters(2 * settings_.startBitrate * 1000, kLossRate,
                                kRtt);
  AddFrame();
}

class TestVideoSenderWithVp8 : public TestVideoSender {
 public:
  TestVideoSenderWithVp8()
      : codec_bitrate_kbps_(300), available_bitrate_kbps_(1000) {}

  void SetUp() override {
    TestVideoSender::SetUp();

    const char* input_video = "foreman_cif";
    const int width = 352;
    const int height = 288;
    generator_.reset(FrameGenerator::CreateFromYuvFile(
        std::vector<std::string>(1, test::ResourcePath(input_video, "yuv")),
        width, height, 1));

    codec_ = MakeVp8VideoCodec(width, height, 3);
    codec_.minBitrate = 10;
    codec_.startBitrate = codec_bitrate_kbps_;
    codec_.maxBitrate = codec_bitrate_kbps_;
    encoder_.reset(VP8Encoder::Create());
    sender_->RegisterExternalEncoder(encoder_.get(), codec_.plType, false);
    EXPECT_EQ(0, sender_->RegisterSendCodec(&codec_, 1, 1200));
  }

  static VideoCodec MakeVp8VideoCodec(int width,
                                      int height,
                                      int temporal_layers) {
    VideoCodec codec;
    VideoCodingModule::Codec(kVideoCodecVP8, &codec);
    codec.width = width;
    codec.height = height;
    codec.codecSpecific.VP8.numberOfTemporalLayers = temporal_layers;
    return codec;
  }

  void InsertFrames(float framerate, float seconds) {
    for (int i = 0; i < seconds * framerate; ++i) {
      clock_.AdvanceTimeMilliseconds(1000.0f / framerate);
      EXPECT_CALL(post_encode_callback_, Encoded(_, NULL, NULL))
          .WillOnce(Return(0));
      AddFrame();
      // SetChannelParameters needs to be called frequently to propagate
      // framerate from the media optimization into the encoder.
      // Note: SetChannelParameters fails if less than 2 frames are in the
      // buffer since it will fail to calculate the framerate.
      if (i != 0) {
        EXPECT_EQ(VCM_OK, sender_->SetChannelParameters(
                              available_bitrate_kbps_ * 1000, 0, 200));
      }
    }
  }

  Vp8StreamInfo SimulateWithFramerate(float framerate) {
    const float short_simulation_interval = 5.0;
    const float long_simulation_interval = 10.0;
    // It appears that this 5 seconds simulation is needed to allow
    // bitrate and framerate to stabilize.
    InsertFrames(framerate, short_simulation_interval);
    packetization_callback_.Reset();

    InsertFrames(framerate, long_simulation_interval);
    return packetization_callback_.CalculateVp8StreamInfo();
  }

 protected:
  VideoCodec codec_;
  int codec_bitrate_kbps_;
  int available_bitrate_kbps_;
};

#if defined(WEBRTC_ANDROID) || defined(WEBRTC_IOS)
#define MAYBE_FixedTemporalLayersStrategy DISABLED_FixedTemporalLayersStrategy
#else
#define MAYBE_FixedTemporalLayersStrategy FixedTemporalLayersStrategy
#endif
TEST_F(TestVideoSenderWithVp8, MAYBE_FixedTemporalLayersStrategy) {
  const int low_b = codec_bitrate_kbps_ * kVp8LayerRateAlloction[2][0];
  const int mid_b = codec_bitrate_kbps_ * kVp8LayerRateAlloction[2][1];
  const int high_b = codec_bitrate_kbps_ * kVp8LayerRateAlloction[2][2];
  {
    Vp8StreamInfo expected = {{7.5, 15.0, 30.0}, {low_b, mid_b, high_b}};
    EXPECT_THAT(SimulateWithFramerate(30.0), MatchesVp8StreamInfo(expected));
  }
  {
    Vp8StreamInfo expected = {{3.75, 7.5, 15.0}, {low_b, mid_b, high_b}};
    EXPECT_THAT(SimulateWithFramerate(15.0), MatchesVp8StreamInfo(expected));
  }
}

#if defined(WEBRTC_ANDROID) || defined(WEBRTC_IOS)
#define MAYBE_RealTimeTemporalLayersStrategy \
  DISABLED_RealTimeTemporalLayersStrategy
#else
#define MAYBE_RealTimeTemporalLayersStrategy RealTimeTemporalLayersStrategy
#endif
TEST_F(TestVideoSenderWithVp8, MAYBE_RealTimeTemporalLayersStrategy) {
  Config extra_options;
  extra_options.Set<TemporalLayers::Factory>(
      new RealTimeTemporalLayersFactory());
  VideoCodec codec = MakeVp8VideoCodec(352, 288, 3);
  codec.extra_options = &extra_options;
  codec.minBitrate = 10;
  codec.startBitrate = codec_bitrate_kbps_;
  codec.maxBitrate = codec_bitrate_kbps_;
  EXPECT_EQ(0, sender_->RegisterSendCodec(&codec, 1, 1200));

  const int low_b = codec_bitrate_kbps_ * 0.4;
  const int mid_b = codec_bitrate_kbps_ * 0.6;
  const int high_b = codec_bitrate_kbps_;

  {
    Vp8StreamInfo expected = {{7.5, 15.0, 30.0}, {low_b, mid_b, high_b}};
    EXPECT_THAT(SimulateWithFramerate(30.0), MatchesVp8StreamInfo(expected));
  }
  {
    Vp8StreamInfo expected = {{5.0, 10.0, 20.0}, {low_b, mid_b, high_b}};
    EXPECT_THAT(SimulateWithFramerate(20.0), MatchesVp8StreamInfo(expected));
  }
  {
    Vp8StreamInfo expected = {{7.5, 15.0, 15.0}, {mid_b, high_b, high_b}};
    EXPECT_THAT(SimulateWithFramerate(15.0), MatchesVp8StreamInfo(expected));
  }
  {
    Vp8StreamInfo expected = {{5.0, 10.0, 10.0}, {mid_b, high_b, high_b}};
    EXPECT_THAT(SimulateWithFramerate(10.0), MatchesVp8StreamInfo(expected));
  }
  {
    // TODO(andresp): Find out why this fails with framerate = 7.5
    Vp8StreamInfo expected = {{7.0, 7.0, 7.0}, {high_b, high_b, high_b}};
    EXPECT_THAT(SimulateWithFramerate(7.0), MatchesVp8StreamInfo(expected));
  }
}
}  // namespace
}  // namespace vcm
}  // namespace webrtc
