// Copyright 2005 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS-IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

// Author: ericv@google.com (Eric Veach)

#include "s2/s2latlng.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include <gtest/gtest.h>

#include "absl/base/macros.h"
#include "absl/container/flat_hash_map.h"
#include "absl/hash/hash_testing.h"
#include "absl/log/log_streamer.h"
#include "absl/random/random.h"
#include "absl/strings/str_cat.h"

#include "s2/s1angle.h"
#include "s2/s2coder_testing.h"
#include "s2/s2error.h"
#include "s2/s2point.h"
#include "s2/s2pointutil.h"
#include "s2/s2random.h"
#include "s2/s2testing.h"

using absl::StrCat;
using std::fabs;
using std::signbit;
using std::string;

TEST(S2LatLng, TestBasic) {
  S2LatLng ll_rad = S2LatLng::FromRadians(M_PI_4, M_PI_2);
  EXPECT_EQ(M_PI_4, ll_rad.lat().radians());
  EXPECT_EQ(M_PI_2, ll_rad.lng().radians());
  EXPECT_TRUE(ll_rad.is_valid());
  S2LatLng ll_deg = S2LatLng::FromDegrees(45, 90);
  EXPECT_EQ(ll_rad, ll_deg);
  EXPECT_TRUE(ll_deg.is_valid());
  EXPECT_FALSE(S2LatLng::FromDegrees(-91, 0).is_valid());
  EXPECT_FALSE(S2LatLng::FromDegrees(0, 181).is_valid());

  S2LatLng bad = S2LatLng::FromDegrees(120, 200);
  EXPECT_FALSE(bad.is_valid());
  S2LatLng better = bad.Normalized();
  EXPECT_TRUE(better.is_valid());
  EXPECT_EQ(S1Angle::Degrees(90), better.lat());
  EXPECT_DOUBLE_EQ(S1Angle::Degrees(-160).radians(), better.lng().radians());

  bad = S2LatLng::FromDegrees(-100, -360);
  EXPECT_FALSE(bad.is_valid());
  better = bad.Normalized();
  EXPECT_TRUE(better.is_valid());
  EXPECT_EQ(S1Angle::Degrees(-90), better.lat());
  EXPECT_DOUBLE_EQ(0.0, better.lng().radians());

  EXPECT_TRUE((S2LatLng::FromDegrees(10, 20) + S2LatLng::FromDegrees(20, 30)).
              ApproxEquals(S2LatLng::FromDegrees(30, 50)));
  EXPECT_TRUE((S2LatLng::FromDegrees(10, 20) - S2LatLng::FromDegrees(20, 30)).
              ApproxEquals(S2LatLng::FromDegrees(-10, -10)));
  EXPECT_TRUE((0.5 * S2LatLng::FromDegrees(10, 20)).
              ApproxEquals(S2LatLng::FromDegrees(5, 10)));

  // Check that Invalid() returns an invalid point.
  S2LatLng invalid = S2LatLng::Invalid();
  EXPECT_FALSE(invalid.is_valid());

  // Check that the default constructor sets latitude and longitude to 0.
  S2LatLng default_ll;
  EXPECT_TRUE(default_ll.is_valid());
  EXPECT_EQ(0, default_ll.lat().radians());
  EXPECT_EQ(0, default_ll.lng().radians());
}

TEST(S2LatLng, TestConversion) {
  // Test special cases: poles, "date line"
  EXPECT_DOUBLE_EQ(90.0,
                   S2LatLng(S2Point(S2LatLng::FromDegrees(90.0, 65.0)))
                   .lat().degrees());
  EXPECT_EQ(-M_PI_2,
            S2LatLng(S2Point(S2LatLng::FromRadians(-M_PI_2, 1)))
            .lat().radians());
  EXPECT_DOUBLE_EQ(180.0,
                   fabs(S2LatLng(S2Point(S2LatLng::FromDegrees(12.2, 180.0)))
                        .lng().degrees()));
  EXPECT_EQ(M_PI,
            fabs(S2LatLng(S2Point(S2LatLng::FromRadians(0.1, -M_PI)))
                 .lng().radians()));

  // Test a bunch of random points.
  absl::BitGen bitgen(S2Testing::MakeTaggedSeedSeq(
      "TEST_CONVERSION", absl::LogInfoStreamer(__FILE__, __LINE__).stream()));
  for (int i = 0; i < 100000; ++i) {
    S2Point p = s2random::Point(bitgen);
    EXPECT_TRUE(S2::ApproxEquals(p, S2Point(S2LatLng(p)))) << p;
  }
}

bool IsIdentical(double x, double y) {
  return x == y && signbit(x) == signbit(y);
}

TEST(S2LatLng, NegativeZeros) {
  EXPECT_TRUE(IsIdentical(
      S2LatLng::Latitude(S2Point(1., 0., -0.)).radians(), +0.));
  EXPECT_TRUE(IsIdentical(
      S2LatLng::Longitude(S2Point(1., -0., 0.)).radians(), +0.));
  EXPECT_TRUE(IsIdentical(
      S2LatLng::Longitude(S2Point(-1., -0., 0.)).radians(), M_PI));
  EXPECT_TRUE(IsIdentical(
      S2LatLng::Longitude(S2Point(-0., 0., 1.)).radians(), +0.));
  EXPECT_TRUE(IsIdentical(
      S2LatLng::Longitude(S2Point(-0., -0., 1.)).radians(), +0.));
}

TEST(S2LatLng, InfIsInvalid) {
  EXPECT_FALSE(
      S2LatLng::FromDegrees(std::numeric_limits<double>::infinity(), -122)
          .is_valid());
  EXPECT_FALSE(
      S2LatLng::FromDegrees(37, std::numeric_limits<double>::infinity())
          .is_valid());

  // Also check the results of .Normalized()
  EXPECT_FALSE(
      S2LatLng::FromDegrees(std::numeric_limits<double>::infinity(), -122)
          .Normalized()
          .is_valid());
  EXPECT_FALSE(
      S2LatLng::FromDegrees(37, std::numeric_limits<double>::infinity())
          .Normalized()
          .is_valid());
}

TEST(S2LatLng, NanIsInvalid) {
  EXPECT_FALSE(
      S2LatLng::FromDegrees(std::numeric_limits<double>::quiet_NaN(), -122)
          .is_valid());
  EXPECT_FALSE(
      S2LatLng::FromDegrees(std::numeric_limits<double>::quiet_NaN(), -122)
          .is_valid());

  // Also check the results of .Normalized()
  EXPECT_FALSE(
      S2LatLng::FromDegrees(37, std::numeric_limits<double>::quiet_NaN())
          .Normalized()
          .is_valid());
  EXPECT_FALSE(
      S2LatLng::FromDegrees(37, std::numeric_limits<double>::quiet_NaN())
          .Normalized()
          .is_valid());
}

TEST(S2LatLng, TestDistance) {
  EXPECT_EQ(0.0,
            S2LatLng::FromDegrees(90, 0).GetDistance(
                S2LatLng::FromDegrees(90, 0)).radians());
  EXPECT_NEAR(77.0,
              S2LatLng::FromDegrees(-37, 25).GetDistance(
                  S2LatLng::FromDegrees(-66, -155)).degrees(),
              1e-13);
  EXPECT_NEAR(115.0,
              S2LatLng::FromDegrees(0, 165).GetDistance(
                  S2LatLng::FromDegrees(0, -80)).degrees(),
              1e-13);
  EXPECT_NEAR(180.0,
              S2LatLng::FromDegrees(47, -127).GetDistance(
                  S2LatLng::FromDegrees(-47, 53)).degrees(),
              2e-6);
}

TEST(S2LatLng, TestToString) {
  struct {
    double lat, lng;
    double expected_lat, expected_lng;
  } values[] = {
    {0, 0, 0, 0},
    {1.5, 91.7, 1.5, 91.7},
    {9.9, -0.31, 9.9, -0.31},
    {sqrt(2), -sqrt(5), 1.414214, -2.236068},
    {91.3, 190.4, 90, -169.6},
    {-100, -710, -90, 10},
  };
  int i = 0;
  for (const auto& v : values) {
    SCOPED_TRACE(StrCat("Iteration ", i++));
    S2LatLng p = S2LatLng::FromDegrees(v.lat, v.lng);
    string output = p.ToStringInDegrees();

    double lat, lng;
    ASSERT_EQ(2, std::sscanf(output.c_str(), "%lf,%lf", &lat, &lng));
    EXPECT_NEAR(v.expected_lat, lat, 1e-8);
    EXPECT_NEAR(v.expected_lng, lng, 1e-8);
  }
}

TEST(S2LatLng, TestHashCode) {
  absl::flat_hash_map<S2LatLng, int, S2LatLngHash> map;
  map[S2LatLng::FromDegrees(0, 10)] = 1;
  map[S2LatLng::FromDegrees(2, 12)] = 2;
  map[S2LatLng::FromDegrees(5, 15)] = 3;
  map[S2LatLng::FromDegrees(7, 17)] = 4;
  map[S2LatLng::FromDegrees(11, 19)] = 5;
  EXPECT_EQ(map.size(), 5);
  EXPECT_EQ(1, map[S2LatLng::FromDegrees(0, 10)]);
  EXPECT_EQ(2, map[S2LatLng::FromDegrees(2, 12)]);
  EXPECT_EQ(3, map[S2LatLng::FromDegrees(5, 15)]);
  EXPECT_EQ(4, map[S2LatLng::FromDegrees(7, 17)]);
  EXPECT_EQ(5, map[S2LatLng::FromDegrees(11, 19)]);
}

TEST(S2LatLng, S2CoderWorks) {
  S2LatLng pnt = S2LatLng::FromDegrees(0, 10);

  S2Error error;
  auto decoded = s2coding::RoundTrip(S2LatLng::Coder(), pnt, error);
  EXPECT_EQ(pnt, decoded);
}

TEST(S2LatLng, SupportsAbslHash) {
  EXPECT_TRUE(absl::VerifyTypeImplementsAbslHashCorrectly({
    S2LatLng(),
    S2LatLng::Invalid(),
    S2LatLng::FromDegrees(0, 0),
    S2LatLng::FromDegrees(0, 10),
    S2LatLng::FromDegrees(2, 12),
    S2LatLng::FromDegrees(90, 180),
    S2LatLng::FromDegrees(90, -180),
    S2LatLng::FromDegrees(-90, 180),
    S2LatLng::FromDegrees(-90, -180),
  }));
}
