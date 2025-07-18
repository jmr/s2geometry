// Copyright 2022 Google Inc. All Rights Reserved.
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


#include "s2/s2shape_nesting_query.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include "absl/flags/flag.h"
#include "absl/log/absl_check.h"
#include "absl/log/absl_log.h"
#include "absl/log/log_streamer.h"
#include "absl/random/random.h"
#include "absl/types/span.h"
#include "s2/mutable_s2shape_index.h"
#include "s2/s1angle.h"
#include "s2/s2debug.h"
#include "s2/s2latlng.h"
#include "s2/s2lax_polygon_shape.h"
#include "s2/s2point.h"
#include "s2/s2polygon.h"
#include "s2/s2shape.h"
#include "s2/s2testing.h"

using absl::Span;
using std::make_unique;
using std::unique_ptr;
using std::vector;

namespace {

struct RingSpec {
  S2LatLng center;
  double radius_deg;
  bool reverse = false;  // Should we reverse the vertex order?
};

// Builds an S2LaxPolygonShape out of nested rings according to given ring
// specs.  Each spec specifies a ring center and a radius in degrees.  The
// radius will be made positive if it's not already, and crossing the poles will
// cause a ABSL_CHECK failure.
//
// Rings are generated in counter-clockwise orientation around their center
// by default, if reverse is specified, then the orientation becomes clockwise.
unique_ptr<S2LaxPolygonShape> RingShape(int vertices_per_loop,
                                        Span<const RingSpec> ring_specs) {
  const double radian_step = 2 * M_PI / vertices_per_loop;

  vector<vector<S2Point>> loops;
  for (const RingSpec& spec : ring_specs) {
    // Check that we're not in reach of the poles.
    double radius = std::fabs(spec.radius_deg);
    ABSL_CHECK_LT(spec.center.lat().degrees() + radius, +90);
    ABSL_CHECK_GT(spec.center.lat().degrees() - radius, -90);

    vector<S2Point> vertices(vertices_per_loop);
    for (int i = 0; i < vertices_per_loop; ++i) {
      double angle = i * radian_step;
      S2LatLng pnt =
          S2LatLng::FromDegrees(radius * sin(angle), radius * cos(angle));
      vertices[i] = (spec.center + pnt).Normalized().ToPoint();
    }

    if (spec.reverse) {
      std::reverse(vertices.begin(), vertices.end());
    }

    loops.push_back(std::move(vertices));
  }

  return make_unique<S2LaxPolygonShape>(loops);
}

// Specify a circular arc about a center point.  The arc has thickness and
// extends from the starting angle to the ending angle.
//
// If offset is specified, the result array of points is rotated by that amount
// to change which vertex is first.
//
// By default, the resulting shape is oriented CCW around its center point, but
// if reverse is set, then the points are reversed and its orientation changes
// to CW.
struct ArcSpec {
  S2LatLng center;
  double radius_deg;
  double thickness;
  double start_deg;
  double end_deg;

  // If non-zero, rotate ring vertices by this many points.  The total "real"
  // shift will be offset % vertices_per_loop.
  int offset;

  // Should we reverse order of vertices?
  bool reverse;
};

// Builds an S2LaxPolygonShape from one or more `ArcSpec`s.  Each spec yields an
// arc on a circle made to have the specified thickness. The inner and outer
// edges have their ends connected with a butt cap.  The arcs are generated in
// counter-clockwise order around their center by default.  If reverse is given,
// then the orientation becomes clockwise.
unique_ptr<S2LaxPolygonShape> ArcShape(int vertices_per_loop,
                                       Span<const ArcSpec> specs) {
  auto deg2rad = [](double degrees) { return (M_PI / 180.0) * degrees; };

  vector<vector<S2Point>> loops;
  for (ArcSpec spec : specs) {
    double start_rad = deg2rad(spec.start_deg);
    double end_rad = deg2rad(spec.end_deg);

    ABSL_CHECK_LT(start_rad, end_rad);
    ABSL_CHECK_GT(spec.radius_deg, 0);
    ABSL_CHECK_GT(spec.thickness, 0);
    ABSL_CHECK_EQ(vertices_per_loop % 2, 0);

    const double radius_inner = spec.radius_deg - spec.thickness;
    const double radius_outer = spec.radius_deg + spec.thickness;
    const double radian_step =
        (end_rad - start_rad) / (vertices_per_loop / 2 - 1);

    // Don't allow arcs to go over the poles to avoid the singularity there
    // which makes the resulting logic much more complex.
    ABSL_CHECK_LT(
        spec.center.lat().degrees() + spec.radius_deg + spec.thickness, +90);
    ABSL_CHECK_GT(
        spec.center.lat().degrees() - spec.radius_deg - spec.thickness, -90);

    // Generate inner and outer edges at the same time with implied butt joint.
    vector<S2Point> vertices(vertices_per_loop);
    for (int i = 0; i < vertices_per_loop / 2; ++i) {
      double angle = start_rad + i * radian_step;
      double sina = sin(angle);
      double cosa = cos(angle);

      S2LatLng pnt0 =
          S2LatLng::FromDegrees(radius_outer * sina, radius_outer * cosa);
      S2LatLng pnt1 =
          S2LatLng::FromDegrees(radius_inner * sina, radius_inner * cosa);

      vertices[i] = (spec.center + pnt0).Normalized().ToPoint();
      vertices[vertices_per_loop - i - 1] =
          (spec.center + pnt1).Normalized().ToPoint();
    }

    if (spec.offset) {
      int offset = spec.offset % vertices_per_loop;
      std::rotate(vertices.begin(), vertices.begin() + offset, vertices.end());
    }

    if (spec.reverse) {
      std::reverse(vertices.begin(), vertices.end());
    }

    loops.emplace_back(std::move(vertices));
  }
  return make_unique<S2LaxPolygonShape>(loops);
}
}  // namespace

TEST(S2ShapeNestingQuery, OneChainAlwaysShell) {
  constexpr int kNumEdges = 100;

  MutableS2ShapeIndex index;
  int id =
      index.Add(RingShape(kNumEdges, {{S2LatLng::FromDegrees(0.0, 0.0), 1.0}}));

  S2ShapeNestingQuery query(&index);
  vector<S2ShapeNestingQuery::ChainRelation> relations =
      query.ComputeShapeNesting(id);
  EXPECT_EQ(relations.size(), 1);
  EXPECT_TRUE(relations[0].is_shell());
  EXPECT_FALSE(relations[0].is_hole());
  EXPECT_LT(relations[0].parent_id(), 0);
  EXPECT_EQ(relations[0].holes().size(), 0);
}

TEST(S2ShapeNestingQuery, TwoChainsFormPair) {
  constexpr int kNumEdges = 100;
  const S2LatLng kCenter = S2LatLng::FromDegrees(0.0, 0.0);

  {
    // Nested rings, like a donut.
    MutableS2ShapeIndex index;
    int id = index.Add(
        RingShape(kNumEdges, {{kCenter, 1.0, false}, {kCenter, 0.5, true}}));

    S2ShapeNestingQuery query(&index);
    vector<S2ShapeNestingQuery::ChainRelation> relations =
        query.ComputeShapeNesting(id);

    // First chain should be a shell and the second a hole.
    EXPECT_EQ(relations.size(), 2);
    EXPECT_TRUE(relations[0].is_shell());
    EXPECT_TRUE(relations[1].is_hole());
    EXPECT_FALSE(relations[0].is_hole());
    EXPECT_FALSE(relations[1].is_shell());

    // First chain should have no parent and one hole, which is second chain.
    EXPECT_LT(relations[0].parent_id(), 0);
    EXPECT_EQ(relations[0].holes().size(), 1);
    EXPECT_EQ(relations[0].holes()[0], 1);

    // Second chain should have one parent, which is chain zero, and no holes.
    EXPECT_EQ(relations[1].parent_id(), 0);
    EXPECT_EQ(relations[1].holes().size(), 0);
  }

  {
    // Swapping ring ordering shouldn't change anything.
    MutableS2ShapeIndex index;
    int id = index.Add(
        RingShape(kNumEdges, {{kCenter, 0.5, true}, {kCenter, 1.0, false}}));

    S2ShapeNestingQuery query(&index);
    vector<S2ShapeNestingQuery::ChainRelation> relations =
        query.ComputeShapeNesting(id);

    // First chain should be a shell and the second a hole.
    EXPECT_EQ(relations.size(), 2);
    EXPECT_TRUE(relations[0].is_shell());
    EXPECT_TRUE(relations[1].is_hole());
    EXPECT_FALSE(relations[0].is_hole());
    EXPECT_FALSE(relations[1].is_shell());

    // First chain should have no parent and one hole, which is second chain.
    EXPECT_LT(relations[0].parent_id(), 0);
    EXPECT_EQ(relations[0].holes().size(), 1);
    EXPECT_EQ(relations[0].holes()[0], 1);

    // Second chain should have one parent, which is chain zero, and no holes.
    EXPECT_EQ(relations[1].parent_id(), 0);
    EXPECT_EQ(relations[1].holes().size(), 0);
  }

  {
    // If we reverse the vertex order of the rings.  We should end up with two
    // shells since the hole and shell don't face each other.
    MutableS2ShapeIndex index;
    int id = index.Add(
        RingShape(kNumEdges, {{kCenter, 1.0, true}, {kCenter, 0.5, false}}));

    S2ShapeNestingQuery query(&index);
    vector<S2ShapeNestingQuery::ChainRelation> relations =
        query.ComputeShapeNesting(id);

    // Both chains should be a shell with no holes
    EXPECT_EQ(relations.size(), 2);
    for (int i = 0; i < 2; ++i) {
      EXPECT_TRUE(relations[i].is_shell());
      EXPECT_FALSE(relations[i].is_hole());
      EXPECT_LT(relations[i].parent_id(), 0);
      EXPECT_EQ(relations[i].holes().size(), 0);
    }
  }
}

// Test case for two chains with a shared vertex.
TEST(S2ShapeNestingQuery, TwoChainsWithSharedVertex) {
  auto P = [](double lat, double lng) {
    return S2LatLng::FromDegrees(lat, lng).ToPoint();
  };

  // A quadrangle and a pentagon sharing a vertex:
  //                       ┌--*
  //                       |    ╲
  //                       |     *
  //                   ┌---*-----┘
  //                   |   |
  //                   └---┘
  std::vector<S2Point> loop1{P(0, 0), P(0, -1), P(-1, -1), P(-1, 0)};
  std::vector<S2Point> loop2{P(0, 0), P(0, 1), P(1, 2), P(2, 1), P(1, 0)};

  // Check all rotations of the two loops.
  for (size_t i = 0; i < loop1.size(); ++i) {
    for (size_t j = 0; j < loop2.size(); ++j) {
      std::vector<std::unique_ptr<S2Loop>> loops;
      loops.emplace_back(std::make_unique<S2Loop>(loop1));
      loops.emplace_back(std::make_unique<S2Loop>(loop2));

      S2Polygon bow_tie(std::move(loops));
      EXPECT_EQ(bow_tie.GetParent(0), -1);
      EXPECT_EQ(bow_tie.GetParent(1), -1);

      MutableS2ShapeIndex index;
      index.Add(std::make_unique<S2LaxPolygonShape>(bow_tie));

      S2ShapeNestingQuery query(&index);
      vector<S2ShapeNestingQuery::ChainRelation> relations =
          query.ComputeShapeNesting(0);

      // Each chain should be a shell with no holes.
      ASSERT_EQ(relations.size(), 2);
      EXPECT_TRUE(relations[0].is_shell());
      EXPECT_TRUE(relations[1].is_shell());

      std::rotate(begin(loop2), begin(loop2) + 1, end(loop2));
    }
    std::rotate(begin(loop1), begin(loop1) + 1, end(loop1));
  }
}

TEST(S2ShapeNestingQuery, CanSetDatumShellOption) {
  constexpr int kNumEdges = 100;
  const S2LatLng kCenter = S2LatLng::FromDegrees(0.0, 0.0);

  // Nested rings, like a donut.
  MutableS2ShapeIndex index;
  int id = index.Add(
      RingShape(kNumEdges, {{kCenter, 1.0, false}, {kCenter, 0.5, true}}));

  // We should be able to override the default datum shell strategy.
  S2ShapeNestingQuery::Options options;
  options.set_datum_strategy([](const S2Shape*) { return 1; });
  S2ShapeNestingQuery query(&index, options);

  vector<S2ShapeNestingQuery::ChainRelation> relations =
      query.ComputeShapeNesting(id);

  // Second chain should be a shell and the first a hole.
  EXPECT_EQ(relations.size(), 2);
  EXPECT_TRUE(relations[1].is_shell());
  EXPECT_TRUE(relations[0].is_hole());
  EXPECT_FALSE(relations[1].is_hole());
  EXPECT_FALSE(relations[0].is_shell());
}

TEST(S2ShapeNestingQuery, ShellCanHaveMultipleHoles) {
  constexpr int kNumEdges = 16;

  // A ring with four holes in it like a shirt button.
  MutableS2ShapeIndex index;
  int id = index.Add(
      RingShape(kNumEdges, {{S2LatLng::FromDegrees(0.5, 0.5), 2.0},
                            {S2LatLng::FromDegrees(1.0, 0.5), 0.25, true},
                            {S2LatLng::FromDegrees(0.0, 0.5), 0.25, true},
                            {S2LatLng::FromDegrees(0.5, 1.0), 0.25, true},
                            {S2LatLng::FromDegrees(0.5, 0.0), 0.25, true}}));

  S2ShapeNestingQuery query(&index);
  vector<S2ShapeNestingQuery::ChainRelation> relations =
      query.ComputeShapeNesting(id);

  // First chain should be a shell and have four holes.
  EXPECT_EQ(relations.size(), 5);
  EXPECT_TRUE(relations[0].is_shell());
  EXPECT_FALSE(relations[0].is_hole());
  EXPECT_LT(relations[0].parent_id(), 0);
  EXPECT_EQ(relations[0].holes().size(), 4);

  // Chain zero should have the others as holes, and the others should have
  // chain zero as their parent.
  for (int i = 1; i < 5; ++i) {
    EXPECT_EQ(relations[0].holes()[i - 1], i);

    EXPECT_TRUE(relations[i].is_hole());
    EXPECT_FALSE(relations[i].is_shell());
    EXPECT_EQ(relations[i].parent_id(), 0);
    EXPECT_EQ(relations[i].holes().size(), 0);
  }
}

TEST(S2ShapeNestingQuery, ExactPathIsIrrelevant) {
  constexpr int kNumEdges = 32;
  const S2LatLng kCenter = S2LatLng::FromDegrees(0.0, 0.0);

  // The path we take from the datum shell to the inner shell shouldn't matter
  // for the final classification.  So build a set of nested rings that are open
  // on one end (like a 'C'), so that they're highly concave.  Shift the datum
  // ring and other rings a point at a time to get coverage on the various
  // permutations.
  for (int offset0 = 0; offset0 < kNumEdges; ++offset0) {
    for (int offset1 = 0; offset1 < kNumEdges; ++offset1) {
      ABSL_VLOG(1) << "Offset (" << offset0 << "," << offset1 << ")";

      MutableS2ShapeIndex index;
      int id = index.Add(ArcShape(
          //            center radius thickness start end  offset  reverse
          kNumEdges, {{kCenter, 0.3, 0.15, -240.0, 60.0, offset0, false},
                      {kCenter, 0.3, 0.05, -230.0, 50.0, offset1, true},
                      {kCenter, 1.0, 0.15, -85.0, 265.0, offset1, false},
                      {kCenter, 1.0, 0.05, -80.0, 260.0, offset1, true}}));

      S2ShapeNestingQuery query(&index);
      vector<S2ShapeNestingQuery::ChainRelation> relations =
          query.ComputeShapeNesting(id);

      ABSL_CHECK_EQ(relations.size(), 4);
      EXPECT_TRUE(relations[0].is_shell());
      EXPECT_TRUE(relations[1].is_hole());
      EXPECT_EQ(relations[1].parent_id(), 0);
      EXPECT_TRUE(relations[2].is_shell());
      EXPECT_TRUE(relations[3].is_hole());
      EXPECT_EQ(relations[3].parent_id(), 2);
    }
  }
}

struct NestingTestCase {
  int depth;        // How many nested loops to generate
  int first_chain;  // Which nested loop is the first loop in the list
  bool shuffle;     // If true, shuffle the loop order (other than first loop)
};

using NestingTest = testing::TestWithParam<NestingTestCase>;

TEST_P(NestingTest, NestedChainsPartitionCorrectly) {
  constexpr int kNumEdges = 16;
  const S2LatLng kCenter = S2LatLng::FromDegrees(0.0, 0.0);

  const NestingTestCase& test_case = GetParam();

  vector<RingSpec> rings;
  rings.push_back(RingSpec{kCenter, 2.0 / (test_case.first_chain + 1),
                           test_case.first_chain % 2 == 1});

  for (int i = 0; i < test_case.depth; ++i) {
    if (i == test_case.first_chain) {
      continue;
    }
    rings.push_back(RingSpec{kCenter, 2.0 / (i + 1), i % 2 == 1});
  }

  if (test_case.shuffle) {
    absl::BitGen bitgen(S2Testing::MakeTaggedSeedSeq(
        "NESTED_CHAINS_PARTITION_CORRECTLY", absl::LogInfoStreamer(__FILE__, __LINE__).stream()));
    std::shuffle(rings.begin() + 1, rings.end(), bitgen);
  }

  MutableS2ShapeIndex index;
  int id = index.Add(RingShape(kNumEdges, rings));

  S2ShapeNestingQuery query(&index);
  vector<S2ShapeNestingQuery::ChainRelation> relations =
      query.ComputeShapeNesting(id);
  ABSL_CHECK_EQ(relations.size(), test_case.depth);

  // In the case of the outer ring being the first chain, and no shuffling,
  // then the outer chain should be a shell and then we alternate hole shell
  // as we move inwards.
  if (test_case.first_chain == 0 && !test_case.shuffle) {
    EXPECT_TRUE(relations[0].is_shell());
    EXPECT_EQ(relations[0].holes().size(), 1);
    EXPECT_EQ(relations[0].holes()[0], 1);

    for (int chain = 1; chain < test_case.depth; chain++) {
      if (chain % 2 == 1) {
        // We expect this chain to be a hole
        EXPECT_TRUE(relations[chain].is_hole());
        EXPECT_FALSE(relations[chain].is_shell());
        EXPECT_EQ(relations[chain].parent_id(), chain - 1);
      } else {
        // We expect this chain to be a shell
        EXPECT_FALSE(relations[chain].is_hole());
        EXPECT_TRUE(relations[chain].is_shell());
        EXPECT_EQ(relations[chain].parent_id(), -1);
      }
    }
  }

  // We should always be able to divide the set of chains into pairs of shells
  // and holes, possibly with one shell left over.
  int num_shells = 0;
  int num_holes = 0;
  for (int chain = 0; chain < test_case.depth; chain++) {
    if (relations[chain].is_shell()) {
      num_shells++;
      for (int child : relations[chain].holes()) {
        EXPECT_EQ(relations[child].parent_id(), chain);
      }
    }

    if (relations[chain].is_hole()) {
      num_holes++;
      int parent = relations[chain].parent_id();
      absl::Span<const int32_t> holes = relations[parent].holes();
      EXPECT_NE(std::find(holes.begin(), holes.end(), chain), holes.end());
    }
  }

  // Everything is a hole or a shell.
  EXPECT_EQ(num_holes + num_shells, test_case.depth);
}

INSTANTIATE_TEST_SUITE_P(
    NestingTests, NestingTest,
    testing::ValuesIn<NestingTestCase>({
        // Test even/odd number of rings, outer ring is first.
        {31, 0, false},
        {32, 0, false},
        {31, 0, true},
        {32, 0, true},

        // Test even/odd number of rings, last ring is first.
        {31, 30, true},
        {32, 31, true},

        // Test even/odd number of rings, intermediate ring is first.
        {31, 31 / 13, true},
        {32, 32 / 13, true},
        {31, 31 / 3, true},
        {32, 32 / 3, true},
    }));

