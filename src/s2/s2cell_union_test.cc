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

#include "s2/s2cell_union.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "absl/hash/hash_testing.h"
#include "absl/log/absl_check.h"
#include "absl/log/log_streamer.h"
#include "absl/random/bit_gen_ref.h"
#include "absl/random/random.h"
#include "absl/strings/str_cat.h"

#include "s2/base/commandlineflags.h"
#include "s2/base/commandlineflags_declare.h"
#include "s2/s1angle.h"
#include "s2/s2cap.h"
#include "s2/s2cell.h"
#include "s2/s2cell_id.h"
#include "s2/s2cell_iterator_join.h"
#include "s2/s2coder_testing.h"
#include "s2/s2edge_distances.h"
#include "s2/s2error.h"
#include "s2/s2metrics.h"
#include "s2/s2point.h"
#include "s2/s2random.h"
#include "s2/s2region_coverer.h"
#include "s2/s2testing.h"
#include "s2/s2text_format.h"
#include "s2/util/coding/coder.h"

S2_DECLARE_bool(s2debug);

S2_DEFINE_int32(iters, 2000, "Number of iterations for tests.");

using absl::StrCat;
using std::max;
using std::min;
using std::string;
using std::vector;

using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::IsFalse;
using ::testing::IsTrue;

class S2CellUnionTestPeer {
 public:
  // Creates a possibly invalid S2CellUnion without any checks.
  static S2CellUnion FromVerbatimNoChecks(vector<S2CellId> cell_ids) {
    return S2CellUnion(std::move(cell_ids), S2CellUnion::VERBATIM);
  }
};

TEST(S2CellUnion, DefaultConstructor) {
  vector<S2CellId> ids;
  S2CellUnion empty(ids);
  EXPECT_TRUE(empty.empty());
}

TEST(S2CellUnion, S2CellIdConstructor) {
  S2CellId face1_id = S2CellId::FromFace(1);
  S2CellUnion face1_union({face1_id});
  EXPECT_EQ(1, face1_union.num_cells());
  EXPECT_EQ(face1_id, face1_union.cell_id(0));
}

TEST(S2CellUnion, WholeSphere) {
  S2CellUnion whole_sphere = S2CellUnion::WholeSphere();
  EXPECT_EQ(whole_sphere.LeafCellsCovered(), 6 * (uint64_t{1} << 60));
  whole_sphere.Expand(0);
  EXPECT_EQ(whole_sphere, S2CellUnion::WholeSphere());
}

TEST(S2CellUnion, DuplicateCellsNotValid) {
  S2CellId id = S2CellId(S2Point(1, 0, 0));
  auto cell_union = S2CellUnionTestPeer::FromVerbatimNoChecks(
      vector<S2CellId>{id, id});
  EXPECT_FALSE(cell_union.IsValid());
}

TEST(S2CellUnion, UnsortedCellsNotValid) {
  S2CellId id = S2CellId(S2Point(1, 0, 0)).parent(10);
  auto cell_union = S2CellUnionTestPeer::FromVerbatimNoChecks(
      vector<S2CellId>{id, id.prev()});
  EXPECT_FALSE(cell_union.IsValid());
}

TEST(S2CellUnion, InvalidCellIdNotValid) {
  ASSERT_FALSE(S2CellId::None().is_valid());
  auto cell_union =
      S2CellUnionTestPeer::FromVerbatimNoChecks({S2CellId::None()});
  EXPECT_FALSE(cell_union.IsValid());
}

TEST(S2CellUnion, InvalidCellIdNotValidWithDebugFlag) {
  // Manually save and restore flag, to preserve test state in opensource
  // without gflags.
  const bool saved_s2debug = absl::GetFlag(FLAGS_s2debug);
  absl::SetFlag(&FLAGS_s2debug, false);
  ASSERT_FALSE(S2CellId::None().is_valid());
  auto cell_union = S2CellUnion::FromVerbatim({S2CellId::None()});
  EXPECT_FALSE(cell_union.IsValid());
  absl::SetFlag(&FLAGS_s2debug, saved_s2debug);
}

TEST(S2CellUnion, IsNormalized) {
  S2CellId id = S2CellId(S2Point(1, 0, 0)).parent(10);
  auto cell_union = S2CellUnion::FromVerbatim(
      vector<S2CellId>{id.child(0), id.child(1), id.child(2), id.child(3)});
  EXPECT_TRUE(cell_union.IsValid());
  EXPECT_FALSE(cell_union.IsNormalized());
}

static void AddCells(absl::BitGenRef bitgen, S2CellId id, bool selected,
                     vector<S2CellId>* input, vector<S2CellId>* expected) {
  // Decides whether to add "id" and/or some of its descendants to the
  // test case.  If "selected" is true, then the region covered by "id"
  // *must* be added to the test case (either by adding "id" itself, or
  // some combination of its descendants, or both).  If cell ids are to
  // the test case "input", then the corresponding expected result after
  // simplification is added to "expected".

  if (id == S2CellId::None()) {
    // Initial call: decide whether to add cell(s) from each face.
    for (int face = 0; face < 6; ++face) {
      AddCells(bitgen, S2CellId::FromFace(face), false, input, expected);
    }
    return;
  }
  if (id.is_leaf()) {
    // The `absl::Bernoulli()` call below ensures that the parent of
    // a leaf cell will always be selected (if we make it that far down the
    // hierarchy).
    ABSL_DCHECK(selected);
    input->push_back(id);
    return;
  }
  // The following code ensures that the probability of selecting a cell
  // at each level is approximately the same, i.e. we test normalization
  // of cells at all levels.
  if (!selected &&
      absl::Bernoulli(bitgen, 1.0 / (S2CellId::kMaxLevel - id.level()))) {
    // Once a cell has been selected, the expected output is predetermined.
    // We then make sure that cells are selected that will normalize to
    // the desired output.
    if (expected != nullptr) expected->push_back(id);
    selected = true;
  }

  // With the `absl::Bernoulli()` constants below, this function
  // adds an average of 5/6 * (kMaxLevel - level) cells to "input" where "level"
  // is the level at which the cell was first selected (level 15 on average).
  // Therefore the average number of input cells in a test case is about
  // (5/6 * 15 * 6) = 75.  The average number of output cells is about 6.

  // If a cell is selected, we add it to "input" with probability 5/6.
  bool added = false;
  if (selected && absl::Bernoulli(bitgen, 5.0 / 6)) {
    input->push_back(id);
    added = true;
  }
  int num_children = 0;
  S2CellId child = id.child_begin();
  for (int pos = 0; pos < 4; ++pos, child = child.next()) {
    // If the cell is selected, on average we recurse on 4/12 = 1/3 child.
    // This intentionally may result in a cell and some of its children
    // being included in the test case.
    //
    // If the cell is not selected, on average we recurse on one child.
    // We also make sure that we do not recurse on all 4 children, since
    // then we might include all 4 children in the input case by accident
    // (in which case the expected output would not be correct).
    if (absl::Bernoulli(bitgen, 1.0 / (selected ? 12 : 4)) &&
        num_children < 3) {
      AddCells(bitgen, child, selected, input, expected);
      ++num_children;
    }
    // If this cell was selected but the cell itself was not added, we
    // must ensure that all 4 children (or some combination of their
    // descendants) are added.
    if (selected && !added) AddCells(bitgen, child, selected, input, expected);
  }
}

TEST(S2CellUnion, AddCellsSizes) {
  // Try a bunch of random test cases, and keep track of average
  // statistics for normalization (to see if they agree with the
  // analysis above).
  absl::BitGen bitgen(S2Testing::MakeTaggedSeedSeq(
      "ADD_CELLS_SIZES", absl::LogInfoStreamer(__FILE__, __LINE__).stream()));
  double in_sum = 0, out_sum = 0;
  const int num_iters = absl::GetFlag(FLAGS_iters);
  for (int i = 0; i < num_iters; ++i) {
    vector<S2CellId> input, expected;
    AddCells(bitgen, S2CellId::None(), /*selected=*/false, &input, &expected);
    in_sum += input.size();
    out_sum += expected.size();
  }
  absl::PrintF("avg in %.2f, avg out %.2f\n", in_sum / num_iters,
               out_sum / num_iters);
}

TEST(S2CellUnion, ContainsExpectedCells) {
  absl::BitGen bitgen(S2Testing::MakeTaggedSeedSeq(
      "CONTAINS_EXPECTED_CELLS", absl::LogInfoStreamer(__FILE__, __LINE__).stream()));
  const int num_iters = absl::GetFlag(FLAGS_iters);
  for (int i = 0; i < num_iters; ++i) {
    vector<S2CellId> input, expected;
    AddCells(bitgen, S2CellId::None(), /*selected=*/false, &input, &expected);
    S2CellUnion cellunion(input);
    EXPECT_THAT(cellunion.cell_ids(), Eq(expected));
  }
}

TEST(S2CellUnion, ContainsInputCells) {
  absl::BitGen bitgen(S2Testing::MakeTaggedSeedSeq(
      "CONTAINS_INPUT_CELLS", absl::LogInfoStreamer(__FILE__, __LINE__).stream()));
  const int num_iters = absl::GetFlag(FLAGS_iters);
  for (int i = 0; i < num_iters; ++i) {
    vector<S2CellId> input, expected;
    AddCells(bitgen, S2CellId::None(), /*selected=*/false, &input,
             /*expected=*/nullptr);
    S2CellUnion cellunion(input);

    // Test Contains(S2CellId) and Intersects(S2CellId).
    for (S2CellId input_id : input) {
      EXPECT_TRUE(cellunion.Contains(input_id));
      EXPECT_TRUE(cellunion.Contains(input_id.ToPoint()));
      EXPECT_TRUE(cellunion.Intersects(input_id));
      if (!input_id.is_face()) {
        EXPECT_TRUE(cellunion.Intersects(input_id.parent()));
        if (input_id.level() > 1) {
          EXPECT_TRUE(cellunion.Intersects(input_id.parent().parent()));
          EXPECT_TRUE(cellunion.Intersects(input_id.parent(0)));
        }
      }
      if (!input_id.is_leaf()) {
        EXPECT_TRUE(cellunion.Contains(input_id.child_begin()));
        EXPECT_TRUE(cellunion.Intersects(input_id.child_begin()));
        EXPECT_TRUE(cellunion.Contains(input_id.child_end().prev()));
        EXPECT_TRUE(cellunion.Intersects(input_id.child_end().prev()));
        EXPECT_TRUE(cellunion.Contains(
                        input_id.child_begin(S2CellId::kMaxLevel)));
        EXPECT_TRUE(cellunion.Intersects(
                        input_id.child_begin(S2CellId::kMaxLevel)));
      }
    }
  }
}

TEST(S2CellUnion, DoesNotContainParentsOfExpectedCell) {
  absl::BitGen bitgen(
      S2Testing::MakeTaggedSeedSeq("DOES_NOT_CONTAIN_PARENTS_OF_EXPECTED_CELL",
                              absl::LogInfoStreamer(__FILE__, __LINE__).stream()));
  const int num_iters = absl::GetFlag(FLAGS_iters);
  for (int i = 0; i < num_iters; ++i) {
    vector<S2CellId> input, expected;
    AddCells(bitgen, S2CellId::None(), /*selected=*/false, &input, &expected);
    S2CellUnion cellunion(input);

    for (S2CellId expected_id : expected) {
      if (!expected_id.is_face()) {
        EXPECT_TRUE(!cellunion.Contains(expected_id.parent()));
        EXPECT_TRUE(!cellunion.Contains(expected_id.parent(0)));
      }
    }
  }
}

TEST(S2CellUnion, UnionIsOr) {
  absl::BitGen bitgen(
      S2Testing::MakeTaggedSeedSeq("UNION_IS_OR", absl::LogInfoStreamer(__FILE__, __LINE__).stream()));
  const int num_iters = absl::GetFlag(FLAGS_iters);
  for (int i = 0; i < num_iters; ++i) {
    vector<S2CellId> input;
    AddCells(bitgen, S2CellId::None(), /*selected=*/false, &input,
             /*expected=*/nullptr);
    S2CellUnion cellunion(input);

    vector<S2CellId> x, y, x_or_y;
    for (S2CellId input_id : input) {
      bool in_x = absl::Bernoulli(bitgen, 0.5);
      bool in_y = absl::Bernoulli(bitgen, 0.5);
      if (in_x) x.push_back(input_id);
      if (in_y) y.push_back(input_id);
      if (in_x || in_y) x_or_y.push_back(input_id);
    }
    S2CellUnion xcells(std::move(x));
    S2CellUnion ycells(std::move(y));
    S2CellUnion x_or_y_expected(std::move(x_or_y));
    S2CellUnion x_or_y_cells = xcells.Union(ycells);
    EXPECT_THAT(x_or_y_cells, Eq(x_or_y_expected));
  }
}

TEST(S2CellUnion, IntersectionIsAnd) {
  absl::BitGen bitgen(S2Testing::MakeTaggedSeedSeq(
      "INTERSECTION_IS_AND", absl::LogInfoStreamer(__FILE__, __LINE__).stream()));
  const int num_iters = absl::GetFlag(FLAGS_iters);
  for (int i = 0; i < num_iters; ++i) {
    vector<S2CellId> input;
    AddCells(bitgen, S2CellId::None(), /*selected=*/false, &input,
             /*expected=*/nullptr);
    S2CellUnion cellunion(input);

    vector<S2CellId> x, y, x_and_y;
    for (S2CellId input_id : input) {
      if (absl::Bernoulli(bitgen, 0.5)) x.push_back(input_id);
      if (absl::Bernoulli(bitgen, 0.5)) y.push_back(input_id);
    }
    S2CellUnion xcells(std::move(x));
    S2CellUnion ycells(std::move(y));

    // Compute the intersection of "x" with each cell of "y",
    // check that this intersection is correct, and append the
    // results to x_and_y_expected.
    for (S2CellId yid : ycells) {
      S2CellUnion ucells = xcells.Intersection(yid);
      for (S2CellId xid : xcells) {
        if (xid.contains(yid)) {
          // TODO(user): Log `input` via `LogContainer`?  This dep
          // currently only comes in via `compact_array`.
          EXPECT_THAT(ucells.cell_ids(), ElementsAre(yid))
              << "xid: " << xid << " yid: " << yid << " xcells: " << xcells
              << " ycells: " << ycells;
        } else if (yid.contains(xid)) {
          EXPECT_TRUE(ucells.Contains(xid))
              << "xid: " << xid << " yid: " << yid << " xcells: " << xcells
              << " ycells: " << ycells << " ucells: " << ucells;
        }
      }
      for (S2CellId uid : ucells) {
        EXPECT_TRUE(xcells.Contains(uid))
            << "xcells: " << xcells << " uid: " << uid;
        EXPECT_TRUE(yid.contains(uid))
            << "xcells: " << s2textformat::ToString(xcells)
            << " ucells: " << s2textformat::ToString(ucells)
            << " yid: " << yid.ToToken() << " uid: " << uid.ToToken();
      }
      x_and_y.insert(x_and_y.end(), ucells.begin(), ucells.end());
    }
    S2CellUnion x_and_y_expected(std::move(x_and_y));
    S2CellUnion x_and_y_cells = xcells.Intersection(ycells);
    EXPECT_THAT(x_and_y_cells, Eq(x_and_y_expected));
  }
}

TEST(S2CellUnion, IntersectionWithCellIdNotInUnionIsEmpty) {
  S2CellUnion xcells =
      s2textformat::MakeCellUnionOrDie("2/1021322000001121003");
  S2CellId yid = s2textformat::MakeCellIdOrDie("2/10213223");
  S2CellUnion ucells = xcells.Intersection(yid);
  EXPECT_THAT(ucells.cell_ids(), IsEmpty());
  EXPECT_FALSE(ucells.Contains(yid));
}

TEST(S2CellUnion, DifferenceIsXAndNotY) {
  absl::BitGen bitgen(S2Testing::MakeTaggedSeedSeq(
      "DIFFERENCE_IS_X_AND_NOT_Y", absl::LogInfoStreamer(__FILE__, __LINE__).stream()));
  const int num_iters = absl::GetFlag(FLAGS_iters);
  for (int i = 0; i < num_iters; ++i) {
    vector<S2CellId> input;
    AddCells(bitgen, S2CellId::None(), /*selected=*/false, &input,
             /*expected=*/nullptr);
    S2CellUnion cellunion(input);

    vector<S2CellId> x, y;
    for (S2CellId input_id : input) {
      if (absl::Bernoulli(bitgen, 0.5)) x.push_back(input_id);
      if (absl::Bernoulli(bitgen, 0.5)) y.push_back(input_id);
    }
    S2CellUnion xcells(std::move(x));
    S2CellUnion ycells(std::move(y));

    S2CellUnion x_minus_y_cells = xcells.Difference(ycells);
    S2CellUnion y_minus_x_cells = ycells.Difference(xcells);
    EXPECT_TRUE(xcells.Contains(x_minus_y_cells));
    EXPECT_TRUE(!x_minus_y_cells.Intersects(ycells));
    EXPECT_TRUE(ycells.Contains(y_minus_x_cells));
    EXPECT_TRUE(!y_minus_x_cells.Intersects(xcells));
    EXPECT_TRUE(!x_minus_y_cells.Intersects(y_minus_x_cells));
  }
}

TEST(S2CellUnion, DifferenceInclusionExclusion) {
  absl::BitGen bitgen(S2Testing::MakeTaggedSeedSeq(
      "DIFFERENCE_INCLUSION_EXCLUSION", absl::LogInfoStreamer(__FILE__, __LINE__).stream()));
  const int num_iters = absl::GetFlag(FLAGS_iters);
  for (int i = 0; i < num_iters; ++i) {
    vector<S2CellId> input;
    AddCells(bitgen, S2CellId::None(), /*selected=*/false, &input,
             /*expected=*/nullptr);
    S2CellUnion cellunion(input);

    vector<S2CellId> x, y;
    for (S2CellId input_id : input) {
      if (absl::Bernoulli(bitgen, 0.5)) x.push_back(input_id);
      if (absl::Bernoulli(bitgen, 0.5)) y.push_back(input_id);
    }
    S2CellUnion xcells(std::move(x));
    S2CellUnion ycells(std::move(y));

    S2CellUnion x_minus_y_cells = xcells.Difference(ycells);
    S2CellUnion y_minus_x_cells = ycells.Difference(xcells);
    S2CellUnion x_and_y_cells = xcells.Intersection(ycells);
    S2CellUnion x_or_y_cells = xcells.Union(ycells);

    S2CellUnion diff_intersection_union =
        x_minus_y_cells.Union(y_minus_x_cells).Union(x_and_y_cells);
    EXPECT_THAT(diff_intersection_union, Eq(x_or_y_cells));
  }
}

TEST(S2CellUnion, ContainsIntersectsBruteForce) {
  absl::BitGen bitgen(S2Testing::MakeTaggedSeedSeq(
      "CONTAINS_INTERSECTS_BRUTE_FORCE", absl::LogInfoStreamer(__FILE__, __LINE__).stream()));
  const int num_iters = absl::GetFlag(FLAGS_iters);
  for (int i = 0; i < num_iters; ++i) {
    vector<S2CellId> input, expected;
    AddCells(bitgen, S2CellId::None(), /*selected=*/false, &input, &expected);
    S2CellUnion cellunion(input);
    // Generate some new cells and see if `Contains` / `Intersects` agree
    // with a brute-force computation.
    vector<S2CellId> test;
    AddCells(bitgen, S2CellId::None(), /*selected=*/false, &test,
             /*expected=*/nullptr);
    for (S2CellId test_id : test) {
      bool contains = false, intersects = false;
      for (S2CellId expected_id : expected) {
        if (expected_id.contains(test_id)) contains = true;
        if (expected_id.intersects(test_id)) intersects = true;
      }
      EXPECT_EQ(contains, cellunion.Contains(test_id));
      EXPECT_EQ(intersects, cellunion.Intersects(test_id));
    }
  }
}

TEST(S2CellUnion, CapBoundContainsAllCells) {
  absl::BitGen bitgen(S2Testing::MakeTaggedSeedSeq(
      "CAP_BOUNDS_CONTAINS_ALL_CELLS", absl::LogInfoStreamer(__FILE__, __LINE__).stream()));
  const int num_iters = absl::GetFlag(FLAGS_iters);
  for (int i = 0; i < num_iters; ++i) {
    vector<S2CellId> input;
    AddCells(bitgen, S2CellId::None(), /*selected=*/false,  //
             &input, /*expected=*/nullptr);
    S2CellUnion cellunion(input);
    S2Cap cap = cellunion.GetCapBound();
    for (S2CellId id : cellunion) {
      EXPECT_TRUE(cap.Contains(S2Cell(id)))
          << "cap: " << cap << " cellunion: " << cellunion << " id: " << id
          << " iter: " << i;
    }
  }
}

// Return the maximum geodesic distance from "axis" to any point of
// "covering".
static double GetRadius(const S2CellUnion& covering, const S2Point& axis) {
  double max_dist = 0;
  for (S2CellId id : covering) {
    S2Cell cell(id);
    for (int j = 0; j < 4; ++j) {
      S2Point a = cell.GetVertex(j);
      S2Point b = cell.GetVertex(j + 1);
      double dist;
      // The maximum distance is not always attained at a cell vertex: if at
      // least one vertex is in the opposite hemisphere from "axis" then the
      // maximum may be attained along an edge.  We solve this by computing
      // the minimum distance from the edge to (-axis) instead.  We can't
      // simply do this all the time because S2::GetDistance() has
      // poor accuracy when the result is close to Pi.
      //
      // TODO(ericv): Improve S2::GetDistance() accuracy near Pi.
      if (a.Angle(axis) > M_PI_2 || b.Angle(axis) > M_PI_2) {
        dist = M_PI - S2::GetDistance(-axis, a, b).radians();
      } else {
        dist = a.Angle(axis);
      }
      max_dist = max(max_dist, dist);
    }
  }
  return max_dist;
}

TEST(S2CellUnion, Expand) {
  // This test generates coverings for caps of random sizes, expands
  // the coverings by a random radius, and then make sure that the new
  // covering covers the expanded cap.  It also makes sure that the
  // new covering is not too much larger than expected.
  absl::BitGen bitgen(
      S2Testing::MakeTaggedSeedSeq("EXPAND", absl::LogInfoStreamer(__FILE__, __LINE__).stream()));

  S2RegionCoverer coverer;
  const int num_iters = absl::GetFlag(FLAGS_iters);
  for (int i = 0; i < num_iters; ++i) {
    SCOPED_TRACE(StrCat("Iteration ", i));
    S2Cap cap = s2random::Cap(bitgen, S2Cell::AverageArea(S2CellId::kMaxLevel),
                              4 * M_PI);

    // Expand the cap area by a random factor whose log is uniformly
    // distributed between 0 and log(1e2).
    S2Cap expanded_cap = S2Cap::FromCenterHeight(
        cap.center(),
        min(2.0, s2random::LogUniform(bitgen, 1.0, 1e2) * cap.height()));

    double radius = (expanded_cap.GetRadius() - cap.GetRadius()).radians();
    int max_level_diff = absl::Uniform(bitgen, 0, 8);

    // Generate a covering for the original cap, and measure the maximum
    // distance from the cap center to any point in the covering.
    coverer.mutable_options()->set_max_cells(1 +
                                             s2random::SkewedInt(bitgen, 10));
    S2CellUnion covering = coverer.GetCovering(cap);
    S2Testing::CheckCovering(cap, covering, true);
    double covering_radius = GetRadius(covering, cap.center());

    // This code duplicates the logic in Expand(min_radius, max_level_diff)
    // that figures out an appropriate cell level to use for the expansion.
    int min_level = S2CellId::kMaxLevel;
    for (S2CellId id : covering) {
      min_level = min(min_level, id.level());
    }
    int expand_level = min(min_level + max_level_diff,
                           S2::kMinWidth.GetLevelForMinValue(radius));

    // Generate a covering for the expanded cap, and measure the new maximum
    // distance from the cap center to any point in the covering.
    covering.Expand(S1Angle::Radians(radius), max_level_diff);
    S2Testing::CheckCovering(expanded_cap, covering, false);
    double expanded_covering_radius = GetRadius(covering, cap.center());

    // If the covering includes a tiny cell along the boundary, in theory the
    // maximum angle of the covering from the cap center can increase by up to
    // twice the maximum length of a cell diagonal.
    EXPECT_LE(expanded_covering_radius - covering_radius,
              2 * S2::kMaxDiag.GetValue(expand_level));
  }
}

TEST(S2CellUnion, EncodeDecode) {
  vector<S2CellId> cell_ids = {S2CellId(0x33),
                               S2CellId(0x8e3748fab),
                               S2CellId(0x91230abcdef83427)};
  auto cell_union = S2CellUnion::FromVerbatim(std::move(cell_ids));

  Encoder encoder;
  cell_union.Encode(&encoder);
  Decoder decoder(encoder.base(), encoder.length());
  S2CellUnion decoded_cell_union;
  EXPECT_TRUE(decoded_cell_union.Decode(&decoder));
  EXPECT_EQ(cell_union, decoded_cell_union);
}

TEST(S2CellUnion, EncodeDecodeEmpty) {
  S2CellUnion empty_cell_union;

  Encoder encoder;
  empty_cell_union.Encode(&encoder);
  Decoder decoder(encoder.base(), encoder.length());
  S2CellUnion decoded_cell_union;
  EXPECT_TRUE(decoded_cell_union.Decode(&decoder));
  EXPECT_EQ(empty_cell_union, decoded_cell_union);
}

static void TestFromMinMax(S2CellId min_id, S2CellId max_id) {
  auto cell_union = S2CellUnion::FromMinMax(min_id, max_id);
  const vector<S2CellId>& cell_ids = cell_union.cell_ids();

  EXPECT_GT(cell_ids.size(), 0);
  EXPECT_EQ(min_id, cell_ids.front().range_min());
  EXPECT_EQ(max_id, cell_ids.back().range_max());
  for (int i = 1; i < cell_ids.size(); ++i) {
    EXPECT_EQ(cell_ids[i].range_min(), cell_ids[i-1].range_max().next());
  }
  EXPECT_TRUE(cell_union.IsNormalized());
}

TEST(S2CellUnion, FromMinMax) {
  // Check the very first leaf cell and face cell.
  S2CellId face1_id = S2CellId::FromFace(0);
  TestFromMinMax(face1_id.range_min(), face1_id.range_min());
  TestFromMinMax(face1_id.range_min(), face1_id.range_max());

  // Check the very last leaf cell and face cell.
  S2CellId face5_id = S2CellId::FromFace(5);
  TestFromMinMax(face5_id.range_min(), face5_id.range_max());
  TestFromMinMax(face5_id.range_max(), face5_id.range_max());

  // Check random ranges of leaf cells.
  absl::BitGen bitgen(S2Testing::MakeTaggedSeedSeq(
      "FROM_MIN_MAX", absl::LogInfoStreamer(__FILE__, __LINE__).stream()));
  for (int iter = 0; iter < 100; ++iter) {
    S2CellId x = s2random::CellId(bitgen, S2CellId::kMaxLevel);
    S2CellId y = s2random::CellId(bitgen, S2CellId::kMaxLevel);
    using std::swap;
    if (x > y) swap(x, y);
    TestFromMinMax(x, y);
  }
}

TEST(S2CellUnion, FromBeginEnd) {
  // Since FromMinMax() is implemented in terms of FromBeginEnd(), we
  // focus on test cases that generate an empty range.
  S2CellId initial_id = S2CellId::FromFace(3);

  // Test an empty range before the minimum S2CellId.
  S2CellUnion cell_union({initial_id});
  S2CellId id_begin = S2CellId::Begin(S2CellId::kMaxLevel);
  cell_union.InitFromBeginEnd(id_begin, id_begin);
  EXPECT_TRUE(cell_union.empty());

  // Test an empty range after the maximum S2CellId.
  cell_union.Init({initial_id});
  S2CellId id_end = S2CellId::End(S2CellId::kMaxLevel);
  cell_union.InitFromBeginEnd(id_end, id_end);
  EXPECT_TRUE(cell_union.empty());

  // Test the full sphere.
  cell_union = S2CellUnion::FromBeginEnd(id_begin, id_end);
  EXPECT_EQ(6, cell_union.num_cells());
  for (S2CellId id : cell_union) {
    EXPECT_TRUE(id.is_face());
  }
}

TEST(S2CellUnion, EmptyMutableOps) {
  S2CellUnion empty_cell_union;

  // Normalize()
  empty_cell_union.Normalize();
  EXPECT_TRUE(empty_cell_union.empty());

  // Denormalize(...)
  vector<S2CellId> output;
  empty_cell_union.Denormalize(0, 2, &output);
  EXPECT_TRUE(empty_cell_union.empty());

  // Pack(...)
  empty_cell_union.Pack();
  EXPECT_TRUE(empty_cell_union.empty());

  // Expand(...)
  empty_cell_union.Expand(S1Angle::Radians(1), 20);
  EXPECT_TRUE(empty_cell_union.empty());
  empty_cell_union.Expand(10);
  EXPECT_TRUE(empty_cell_union.empty());
}

TEST(S2CellUnion, EmptyAndNonEmptyBooleanOps) {
  const S2CellUnion empty_cell_union;
  const S2CellId face1_id = S2CellId::FromFace(1);
  const S2CellUnion non_empty_cell_union({face1_id});

  // Contains(...)
  EXPECT_FALSE(empty_cell_union.Contains(face1_id));
  EXPECT_TRUE(non_empty_cell_union.Contains(face1_id));
  EXPECT_TRUE(empty_cell_union.Contains(empty_cell_union));
  EXPECT_TRUE(non_empty_cell_union.Contains(empty_cell_union));
  EXPECT_FALSE(empty_cell_union.Contains(non_empty_cell_union));
  EXPECT_TRUE(non_empty_cell_union.Contains(non_empty_cell_union));

  // Intersects(...)
  EXPECT_FALSE(empty_cell_union.Intersects(face1_id));
  EXPECT_TRUE(non_empty_cell_union.Intersects(face1_id));
  EXPECT_FALSE(empty_cell_union.Intersects(empty_cell_union));
  EXPECT_FALSE(non_empty_cell_union.Intersects(empty_cell_union));
  EXPECT_FALSE(empty_cell_union.Intersects(non_empty_cell_union));
  EXPECT_TRUE(non_empty_cell_union.Intersects(non_empty_cell_union));

  // Union(...)
  EXPECT_EQ(empty_cell_union, empty_cell_union.Union(empty_cell_union));
  EXPECT_EQ(non_empty_cell_union, non_empty_cell_union.Union(empty_cell_union));
  EXPECT_EQ(non_empty_cell_union, empty_cell_union.Union(non_empty_cell_union));
  EXPECT_EQ(non_empty_cell_union,
            non_empty_cell_union.Union(non_empty_cell_union));

  // Intersection(...)
  EXPECT_EQ(empty_cell_union, empty_cell_union.Intersection(face1_id));
  EXPECT_EQ(non_empty_cell_union, non_empty_cell_union.Intersection(face1_id));
  EXPECT_EQ(empty_cell_union, empty_cell_union.Intersection(empty_cell_union));
  EXPECT_EQ(empty_cell_union,
            non_empty_cell_union.Intersection(empty_cell_union));
  EXPECT_EQ(empty_cell_union,
            empty_cell_union.Intersection(non_empty_cell_union));
  EXPECT_EQ(non_empty_cell_union,
            non_empty_cell_union.Intersection(non_empty_cell_union));

  // Difference(...)
  EXPECT_EQ(empty_cell_union, empty_cell_union.Difference(empty_cell_union));
  EXPECT_EQ(non_empty_cell_union,
            non_empty_cell_union.Difference(empty_cell_union));
  EXPECT_EQ(empty_cell_union,
            empty_cell_union.Difference(non_empty_cell_union));
  EXPECT_EQ(S2CellUnion(),
            non_empty_cell_union.Difference(non_empty_cell_union));
}

TEST(S2CellUnion, Clear) {
  S2CellId face1_id = S2CellId::FromFace(1);
  S2CellUnion face1_union({face1_id});

  ASSERT_EQ(1, face1_union.num_cells());
  EXPECT_EQ(1, face1_union.cell_ids().size());
  EXPECT_LE(1, face1_union.cell_ids().capacity());

  face1_union.Clear();
  ASSERT_EQ(0, face1_union.num_cells());
  EXPECT_EQ(0, face1_union.cell_ids().size());
  EXPECT_EQ(0, face1_union.cell_ids().capacity());
}

TEST(S2CellUnion, RefuseToDecode) {
  vector<S2CellId> cellids;
  S2CellId id = S2CellId::Begin(S2CellId::kMaxLevel);
  for (int i = 0; i <= absl::GetFlag(FLAGS_s2cell_union_decode_max_num_cells);
       ++i) {
    cellids.push_back(id);
    id = id.next();
  }
  S2CellUnion cell_union = S2CellUnion::FromVerbatim(cellids);
  Encoder encoder;
  cell_union.Encode(&encoder);
  Decoder decoder(encoder.base(), encoder.length());
  S2CellUnion decoded_cell_union;
  EXPECT_FALSE(decoded_cell_union.Decode(&decoder));
}

TEST(S2CellUnion, Release) {
  S2CellId face1_id = S2CellId::FromFace(1);
  S2CellUnion face1_union({face1_id});
  ASSERT_EQ(1, face1_union.num_cells());
  EXPECT_EQ(face1_id, face1_union.cell_id(0));

  vector<S2CellId> released = face1_union.Release();
  ASSERT_EQ(1, released.size());
  EXPECT_EQ(face1_id, released[0]);
  EXPECT_EQ(0, face1_union.num_cells());
}

TEST(S2CellUnion, LeafCellsCovered) {
  S2CellUnion cell_union;
  EXPECT_EQ(0, cell_union.LeafCellsCovered());

  vector<S2CellId> ids;
  // One leaf cell on face 0.
  ids.push_back(S2CellId::FromFace(0).child_begin(S2CellId::kMaxLevel));
  cell_union.Init(ids);
  EXPECT_EQ(uint64_t{1}, cell_union.LeafCellsCovered());

  // Face 0 itself (which includes the previous leaf cell).
  ids.push_back(S2CellId::FromFace(0));
  cell_union.Init(ids);
  EXPECT_EQ(uint64_t{1} << 60, cell_union.LeafCellsCovered());
  // Five faces.
  cell_union.Expand(0);
  EXPECT_EQ(uint64_t{5} << 60, cell_union.LeafCellsCovered());
  // Whole world.
  cell_union.Expand(0);
  EXPECT_EQ(uint64_t{6} << 60, cell_union.LeafCellsCovered());

  // Add some disjoint cells.
  ids.push_back(S2CellId::FromFace(1).child_begin(1));
  ids.push_back(S2CellId::FromFace(2).child_begin(2));
  ids.push_back(S2CellId::FromFace(2).child_end(2).prev());
  ids.push_back(S2CellId::FromFace(3).child_begin(14));
  ids.push_back(S2CellId::FromFace(4).child_begin(27));
  ids.push_back(S2CellId::FromFace(4).child_end(15).prev());
  ids.push_back(S2CellId::FromFace(5).child_begin(30));
  cell_union.Init(ids);
  uint64_t expected = 1ULL + (1ULL << 6) + (1ULL << 30) + (1ULL << 32) +
                      (2ULL << 56) + (1ULL << 58) + (1ULL << 60);
  EXPECT_EQ(expected, cell_union.LeafCellsCovered());
}

TEST(S2CellUnion, WorksInContainers) {
  vector<S2CellId> ids(1, S2CellId::FromFace(1));
  S2CellUnion cell_union0(ids);

  // This gives a compilation error if the S2CellUnion is neither movable nor
  // copyable.
  vector<S2CellUnion> union_vector;
  union_vector.push_back(std::move(cell_union0));

  EXPECT_EQ(ids, union_vector.back().cell_ids());
}

TEST(S2CellUnion, ToStringEmpty) {
  EXPECT_EQ(S2CellUnion().ToString(), "Size:0 S2CellIds:");
}

TEST(S2CellUnion, ToStringOneCell) {
  EXPECT_EQ(S2CellUnion({S2CellId::FromFace(1)}).ToString(),
            "Size:1 S2CellIds:3");
}

TEST(S2CellUnion, ToStringTwoCells) {
  EXPECT_EQ(
      S2CellUnion({S2CellId::FromFace(1), S2CellId::FromFace(2)}).ToString(),
      "Size:2 S2CellIds:3,5");
}

TEST(S2CellUnion, ToStringOver500Cells) {
  vector<S2CellId> ids;
  S2CellUnion({S2CellId::FromFace(1)}).Denormalize(6, 1, &ids);  // 4096 cells
  string result = S2CellUnion::FromVerbatim(ids).ToString();
  EXPECT_EQ(std::count(result.begin(), result.end(), ','), 500);
  EXPECT_EQ(result.substr(result.size() - 4), ",...");
}

TEST(S2CellUnion, IntersectionOneInputNormalized) {
  S2CellId id = S2CellId::FromFace(3);  // arbitrary
  S2CellUnion parent({id});
  S2CellUnion children = S2CellUnion::FromVerbatim(
      {id.child(0), id.child(1), id.child(2), id.child(3)});
  S2CellUnion intersection = parent.Intersection(children);
  EXPECT_EQ(intersection, children);
}

TEST(S2CellUnion, SupportsAbslHash) {
  EXPECT_TRUE(absl::VerifyTypeImplementsAbslHashCorrectly(
      {S2CellUnion({S2CellId::FromFace(1)}),
       S2CellUnion({S2CellId::FromFace(1), S2CellId::FromFace(2).child_begin(1),
                    S2CellId::FromFace(3).child_begin(2)})}));
}

TEST(S2CellUnion, IteratorWorks) {
  // Get a cell union of the face cells.
  auto cell_union = S2CellUnion::FromBeginEnd(
      S2CellId::Begin(S2CellId::kMaxLevel), S2CellId::End(S2CellId::kMaxLevel));
  ABSL_CHECK_EQ(cell_union.num_cells(), 6);

  auto iter = S2CellUnion::Iterator(&cell_union);

  EXPECT_THAT(iter.id(), Eq(S2CellId::FromFace(0)));
  EXPECT_THAT(iter.done(), IsFalse());
  EXPECT_THAT(iter.Prev(), IsFalse());

  iter.Next();
  EXPECT_THAT(iter.id(), Eq(S2CellId::FromFace(1)));

  iter.Finish();
  EXPECT_THAT(iter.done(), IsTrue());
  EXPECT_THAT(iter.Prev(), IsTrue());
  EXPECT_THAT(iter.id(), Eq(S2CellId::FromFace(5)));

  iter.Seek(S2CellId::FromFace(3));
  EXPECT_THAT(iter.done(), IsFalse());
  EXPECT_THAT(iter.id(), Eq(S2CellId::FromFace(3)));

  iter.Begin();
  EXPECT_THAT(iter.done(), IsFalse());
  EXPECT_THAT(iter.id(), Eq(S2CellId::FromFace(0)));
}
