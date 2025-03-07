// Copyright 2017 Google Inc. All Rights Reserved.
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

#include "s2/s2shapeutil_contains_brute_force.h"

#include <memory>
#include <string>

#include <gtest/gtest.h>
#include "s2/s1angle.h"
#include "s2/s2lax_polygon_shape.h"
#include "s2/s2lax_polyline_shape.h"
#include "s2/s2loop.h"
#include "s2/s2shape.h"
#include "s2/s2text_format.h"

using s2textformat::MakeLaxPolygonOrDie;
using s2textformat::MakeLaxPolylineOrDie;
using s2textformat::MakePointOrDie;

namespace s2shapeutil {

TEST(ContainsBruteForce, NoInterior) {
  // Defines a polyline that almost entirely encloses the point 0:0.
  // Note, however, that -1e-9 was originally -1e9, and this seems to pass
  // with almost any value, so it's not clear what's being tested here.
  auto polyline = MakeLaxPolylineOrDie("0:0, 0:1, 1:-1, -1:-1, -1e-9:1");
  EXPECT_FALSE(ContainsBruteForce(*polyline, MakePointOrDie("0:0")));
}

TEST(ContainsBruteForce, ContainsReferencePoint) {
  // Checks that ContainsBruteForce agrees with GetReferencePoint.
  auto polygon = MakeLaxPolygonOrDie("0:0, 0:1, 1:-1, -1:-1, -1e-9:1");
  auto ref = polygon->GetReferencePoint();
  EXPECT_EQ(ref.contained, ContainsBruteForce(*polygon, ref.point));
}

TEST(ContainsBruteForce, ConsistentWithS2Loop) {
  // Checks that ContainsBruteForce agrees with S2Loop::Contains().
  auto loop = S2Loop::MakeRegularLoop(MakePointOrDie("89:-179"),
                                      S1Angle::Degrees(10), 100);
  S2Loop::Shape shape(loop.get());
  for (int i = 0; i < loop->num_vertices(); ++i) {
    EXPECT_EQ(loop->Contains(loop->vertex(i)),
              ContainsBruteForce(shape, loop->vertex(i)));
  }
}

}  // namespace s2shapeutil
