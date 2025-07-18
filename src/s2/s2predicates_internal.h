// Copyright 2016 Google Inc. All Rights Reserved.
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
//
// The following functions are not part of the public API.  Currently they are
// only used internally for testing purposes.

#ifndef S2_S2PREDICATES_INTERNAL_H_
#define S2_S2PREDICATES_INTERNAL_H_

#include <limits>

#include "absl/base/casts.h"
#include "s2/_fp_contract_off.h"  // IWYU pragma: keep
#include "s2/s1chord_angle.h"
#include "s2/s2point.h"
#include "s2/s2predicates.h"
#include "s2/util/math/exactfloat/exactfloat.h"
#include "s2/util/math/vector.h"

namespace s2pred {

// Returns 2 ** (-digits).  This could be implemented using "ldexp" except
// that std::ldexp is not constexpr until C++23.
constexpr double epsilon_for_digits(int digits) {
  return (digits < 64 ? 1.0 / (1ULL << digits) :
          epsilon_for_digits(digits - 63) / (1ULL << 63));
}

// Returns the maximum rounding error for arithmetic operations in type T.
// We could simply return 0.5 * numeric_limits<T>::epsilon(), except that some
// platforms implement "long double" using "double-double" arithmetic, and for
// those platforms we need to compute the rounding error manually based on
// numeric_limits<T>::digits (the number of bits of mantissa precision).
template <typename T> constexpr T rounding_epsilon() {
  return epsilon_for_digits(std::numeric_limits<T>::digits);
}

constexpr double DBL_ERR = rounding_epsilon<double>();
constexpr long double LD_ERR = rounding_epsilon<long double>();
constexpr bool kHasLongDouble = (LD_ERR < DBL_ERR);

// Define sqrt(3) as a constant so that we can use it with constexpr.
// Unfortunately we can't use M_SQRT3 because some client libraries define
// this symbol without first checking whether it already exists.
constexpr double kSqrt3 = 1.7320508075688772935274463415058;

using Vector3_ld = Vector3<long double>;
using Vector3_xf = Vector3<ExactFloat>;

inline static Vector3_ld ToLD(const S2Point& x) {
  return Vector3_ld::Cast(x);
}

inline static long double ToLD(double x) {
  return absl::implicit_cast<long double>(x);
}

inline static Vector3_xf ToExact(const S2Point& x) {
  return Vector3_xf::Cast(x);
}

// Efficiently tests whether an ExactFloat vector is (0, 0, 0).
inline static bool IsZero(const Vector3_xf& a) {
  return a[0].sgn() == 0 && a[1].sgn() == 0 && a[2].sgn() == 0;
}

int StableSign(const S2Point& a, const S2Point& b, const S2Point& c);

int ExactSign(const S2Point& a, const S2Point& b, const S2Point& c,
              bool perturb);

int SymbolicallyPerturbedSign(
    const Vector3_xf& a, const Vector3_xf& b,
    const Vector3_xf& c, const Vector3_xf& b_cross_c);

template <class T>
int TriageCompareCosDistances(const Vector3<T>& x,
                              const Vector3<T>& a, const Vector3<T>& b);

template <class T>
int TriageCompareSin2Distances(const Vector3<T>& x,
                               const Vector3<T>& a, const Vector3<T>& b);

int ExactCompareDistances(const Vector3_xf& x,
                          const Vector3_xf& a, const Vector3_xf& b);

int SymbolicCompareDistances(const S2Point& x,
                             const S2Point& a, const S2Point& b);

template <class T>
int TriageCompareSin2Distance(const Vector3<T>& x, const Vector3<T>& y, T r2);

template <class T>
int TriageCompareCosDistance(const Vector3<T>& x, const Vector3<T>& y, T r2);

int ExactCompareDistance(const Vector3_xf& x, const Vector3_xf& y,
                         const ExactFloat& r2);

template <class T>
int TriageCompareEdgeDistance(const Vector3<T>& x, const Vector3<T>& a0,
                              const Vector3<T>& a1, T r2);

int ExactCompareEdgeDistance(const S2Point& x, const S2Point& a0,
                             const S2Point& a1, S1ChordAngle r);

// Computes the sign of a.DotProd(b) and returns it, or 0 if it's within the
// error margin.
template <class T>
int TriageSignDotProd(const Vector3<T>& a, const Vector3<T>& b);

// Returns the sign of a.DotProd(b) using exact arithmetic.
int ExactSignDotProd(const Vector3_xf& a, const Vector3_xf& b);

// Orders intersections along a great circle relative to some reference point.
template <class T>
int TriageIntersectionOrdering(const Vector3<T>& a, const Vector3<T>& b,
                               const Vector3<T>& c, const Vector3<T>& d,
                               const Vector3<T>& m, const Vector3<T>& n);

int ExactIntersectionOrdering(const Vector3_xf& a, const Vector3_xf& b,
                              const Vector3_xf& c, const Vector3_xf& d,
                              const Vector3_xf& m, const Vector3_xf& n);

// Computes location of the intersection of edge AB with great circle N with
// respect to the great circles x and y.
template <class T>
int TriageCircleEdgeIntersectionSign(const Vector3<T>& a, const Vector3<T>& b,
                                     const Vector3<T>& n, const Vector3<T>& x);

int ExactCircleEdgeIntersectionSign(const Vector3_xf& a, const Vector3_xf& b,
                                    const Vector3_xf& n, const Vector3_xf& x);

template <class T>
int TriageCompareEdgeDirections(
    const Vector3<T>& a0, const Vector3<T>& a1,
    const Vector3<T>& b0, const Vector3<T>& b1);

int ExactCompareEdgeDirections(const Vector3_xf& a0, const Vector3_xf& a1,
                               const Vector3_xf& b0, const Vector3_xf& b1);

template <class T>
int TriageEdgeCircumcenterSign(const Vector3<T>& x0, const Vector3<T>& x1,
                               const Vector3<T>& a, const Vector3<T>& b,
                               const Vector3<T>& c, int abc_sign);

int ExactEdgeCircumcenterSign(const Vector3_xf& x0, const Vector3_xf& x1,
                              const Vector3_xf& a, const Vector3_xf& b,
                              const Vector3_xf& c, int abc_sign);

int SymbolicEdgeCircumcenterSign(
    const S2Point& x0, const S2Point& x1,
    const S2Point& a_arg, const S2Point& b_arg, const S2Point& c_arg);

template <class T>
Excluded TriageVoronoiSiteExclusion(const Vector3<T>& a, const Vector3<T>& b,
                                    const Vector3<T>& x0, const Vector3<T>& x1,
                                    T r2);

Excluded ExactVoronoiSiteExclusion(const Vector3_xf& a, const Vector3_xf& b,
                                   const Vector3_xf& x0, const Vector3_xf& x1,
                                   const ExactFloat& r2);
}  // namespace s2pred

#endif  // S2_S2PREDICATES_INTERNAL_H_
