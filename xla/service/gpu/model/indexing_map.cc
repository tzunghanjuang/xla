/* Copyright 2023 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/service/gpu/model/indexing_map.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <numeric>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/types/span.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/IR/AffineExpr.h"  // from @llvm-project
#include "mlir/IR/AffineMap.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/Support/LLVM.h"  // from @llvm-project
#include "xla/service/gpu/model/affine_map_printer.h"
#include "xla/service/gpu/model/indexing_context.h"
#include "tsl/platform/logging.h"  // IWYU pragma: keep

namespace xla {
namespace gpu {
namespace {

using llvm::ArrayRef;
using llvm::SmallBitVector;
using llvm::SmallVector;
using mlir::AffineBinaryOpExpr;
using mlir::AffineConstantExpr;
using mlir::AffineDimExpr;
using mlir::AffineExpr;
using mlir::AffineExprKind;
using mlir::AffineMap;
using mlir::AffineSymbolExpr;
using mlir::getAffineBinaryOpExpr;
using mlir::getAffineConstantExpr;
using mlir::MLIRContext;

int64_t FloorDiv(int64_t dividend, int64_t divisor) {
  return dividend / divisor -
         (((dividend >= 0) != (divisor >= 0) && dividend % divisor) ? 1 : 0);
}

int64_t CeilDiv(int64_t dividend, int64_t divisor) {
  return dividend / divisor +
         (((dividend >= 0) == (divisor >= 0) && dividend % divisor) ? 1 : 0);
}

class AffineExprSimplifier {
 public:
  explicit AffineExprSimplifier(RangeEvaluator* range_evaluator)
      : range_evaluator_(range_evaluator) {}

  // Simplifies the map as much as possible.
  mlir::AffineMap Simplify(mlir::AffineMap affine_map);

  mlir::AffineExpr Simplify(mlir::AffineExpr expr);

 private:
  std::optional<int64_t> GetConstantRhs(mlir::AffineExpr expr,
                                        AffineExprKind kind);

  // Simplifier for mod.
  // - Rewrites (a * 100 + ...) % 100 to (...) % 100
  // - Rewrites a % b to a if a is known to be less than b.
  mlir::AffineExpr RewriteMod(mlir::AffineBinaryOpExpr mod);

  // Simplifier for floordiv.
  // - Rewrites (a * 100 + ...) / 100 to a + (...) / 100
  // - Rewrites a / 100 to 0 when a is known to be less than 100.
  mlir::AffineExpr RewriteFloorDiv(mlir::AffineBinaryOpExpr div);

  mlir::AffineExpr RewriteSum(
      mlir::AffineExpr expr,
      const std::function<mlir::AffineExpr(mlir::AffineExpr)>& map);

  mlir::AffineExpr RewriteSumIf(
      mlir::AffineExpr expr, const std::function<bool(mlir::AffineExpr)>& pred);

  // Attempts to simplify the expression, but doesn't attempt to simplify the
  // result further.
  mlir::AffineExpr SimplifyOnce(mlir::AffineExpr expr);

  // Simplifies the expression using MLIR's simplifier, except for mods.
  mlir::AffineExpr SimplifyWithMlir(mlir::AffineExpr expr, int num_dims,
                                    int num_symbols);

  mlir::AffineMap SimplifyWithMlir(mlir::AffineMap map) {
    llvm::SmallVector<mlir::AffineExpr, 8> exprs;
    for (auto e : map.getResults()) {
      exprs.push_back(
          SimplifyWithMlir(e, map.getNumDims(), map.getNumSymbols()));
    }
    return mlir::AffineMap::get(map.getNumDims(), map.getNumSymbols(), exprs,
                                map.getContext());
  }

  RangeEvaluator* range_evaluator_;
};

AffineExpr AffineExprSimplifier::RewriteMod(AffineBinaryOpExpr mod) {
  auto rhs = range_evaluator_->ComputeExpressionRange(mod.getRHS());

  // The logic below assumes we have a constant RHS.
  if (!rhs.IsPoint()) {
    return mod;
  }
  int64_t m = rhs.lower;
  // Can only happen in cases where it doesn't matter, return 0.
  if (m == 0) {
    return mlir::getAffineConstantExpr(0, mod.getContext());
  }

  auto lhs_simplified = SimplifyOnce(mod.getLHS());
  auto lhs = range_evaluator_->ComputeExpressionRange(lhs_simplified);
  // a % b where b is always larger than a?
  if (0 <= lhs.lower && lhs.upper < rhs.lower) {
    return lhs_simplified;
  }

  // Rewrite `(c * a) % ab` to `(c % b) * a`.
  //   (c * a) % ab
  // = c * a - (c * a) // ab * ab
  // = c * a - c // b * ab
  // = (c - c // b * b) * a
  // = (c % b) * a
  if (auto mul = GetConstantRhs(lhs_simplified, AffineExprKind::Mul);
      mul && (m % *mul == 0)) {
    return (mlir::cast<AffineBinaryOpExpr>(lhs_simplified).getLHS() %
            (m / *mul)) *
           *mul;
  }

  Interval no_multiplier_range{0, 0};
  int64_t multiplier_gcd = -1;

  int64_t extracted_constant = 0;
  auto new_lhs = RewriteSumIf(lhs_simplified, [&](AffineExpr expr) {
    if (auto cst = mlir::dyn_cast<AffineConstantExpr>(expr);
        cst && cst.getValue() >= m) {
      extracted_constant += cst.getValue();
      return false;
    }
    if (auto multiplier = GetConstantRhs(expr, AffineExprKind::Mul)) {
      if (*multiplier % m == 0) {
        return false;
      }

      if (multiplier_gcd == -1) {
        multiplier_gcd = *multiplier;
      } else {
        multiplier_gcd = std::gcd(multiplier_gcd, *multiplier);
      }
      return true;
    }
    auto range = range_evaluator_->ComputeExpressionRange(expr);
    no_multiplier_range.lower += range.lower;
    no_multiplier_range.upper += range.upper;
    return true;
  });
  new_lhs = new_lhs + (extracted_constant % m);

  mlir::AffineExpr extracted = getAffineConstantExpr(0, mod.getContext());
  if (m % multiplier_gcd == 0 && no_multiplier_range.lower >= 0 &&
      no_multiplier_range.upper < multiplier_gcd) {
    // Remove everything that doesn't have a multiplier.
    new_lhs = RewriteSumIf(new_lhs, [&](AffineExpr expr) {
      if (GetConstantRhs(expr, AffineExprKind::Mul)) {
        return true;
      }
      extracted = extracted + expr;
      return false;
    });
  }
  return new_lhs % mod.getRHS() + extracted;
}

AffineExpr AffineExprSimplifier::RewriteFloorDiv(AffineBinaryOpExpr div) {
  auto mlir_context = range_evaluator_->GetMLIRContext();
  auto lhs_simplified = SimplifyOnce(div.getLHS());
  auto lhs = range_evaluator_->ComputeExpressionRange(lhs_simplified);
  auto rhs = range_evaluator_->ComputeExpressionRange(div.getRHS());

  if (0 <= lhs.lower && lhs.upper < rhs.lower) {
    return getAffineConstantExpr(0, mlir_context);
  }

  // The logic below assumes we have a constant RHS.
  if (!rhs.IsPoint()) {
    return div;
  }
  int64_t d = rhs.lower;

  // Rewrite `(c % ab) // a` to `(c // a) % b`.
  //   (c % ab) // a
  // = (c - c // ab * ab) // a               expand mod
  // = c // a - (c // ab * b)                rhs of - divides a
  // = c // a - (c // a) // b * b)           split ab
  // = (c // a) % b                          contract mod
  if (auto mod = GetConstantRhs(lhs_simplified, AffineExprKind::Mod);
      mod && (*mod % d == 0)) {
    return mlir::cast<AffineBinaryOpExpr>(lhs_simplified).getLHS().floorDiv(d) %
           (*mod / d);
  }

  // If the dividend's range has a single element, return its value.
  int64_t a = FloorDiv(lhs.lower, d);
  int64_t b = FloorDiv(lhs.upper, d);
  if (a == b) {
    return getAffineConstantExpr(a, mlir_context);
  }

  // Rewrite `(a / b) / c` to `a / (b * c)` if `a >= 0` and `b` and `c` are
  // constants.
  if (lhs_simplified.getKind() == AffineExprKind::FloorDiv) {
    auto lhs_div = mlir::cast<AffineBinaryOpExpr>(lhs_simplified);
    auto lhs_lhs = range_evaluator_->ComputeExpressionRange(lhs_div.getLHS());
    if (lhs_lhs.lower >= 0) {
      auto lhs_rhs = range_evaluator_->ComputeExpressionRange(lhs_div.getRHS());
      if (lhs_rhs.IsPoint()) {
        return lhs_div.getLHS().floorDiv(lhs_rhs.lower * d);
      }
    }
  }

  Interval no_multiplier_range{0, 0};
  int64_t multiplier_gcd = -1;
  // The maximum GCD of any remaining multiplier inside the div and the divisor.
  int64_t max_remaining_multiplier_gcd = -1;
  AffineExpr zero = getAffineConstantExpr(0, mlir_context);
  AffineExpr extracted = zero;
  auto new_dividend = RewriteSumIf(lhs_simplified, [&](AffineExpr expr) {
    if (auto multiplier = GetConstantRhs(expr, AffineExprKind::Mul)) {
      // (x * 7 + ...) / 3 -> can't extract. We could extract x * 2 and keep
      // one x, but we currently have no reason to do that.

      if (*multiplier % d == 0) {
        int64_t factor = *multiplier / d;
        extracted =
            extracted + mlir::cast<AffineBinaryOpExpr>(expr).getLHS() * factor;
        // Remove from dividend.
        return false;
      }

      if (*multiplier > 0) {
        if (multiplier_gcd == -1) {
          multiplier_gcd = *multiplier;
        } else {
          multiplier_gcd = std::gcd(multiplier_gcd, *multiplier);
        }
        max_remaining_multiplier_gcd =
            std::max(max_remaining_multiplier_gcd, std::gcd(*multiplier, d));
        return true;
      }
    }
    auto range = range_evaluator_->ComputeExpressionRange(expr);
    no_multiplier_range.lower += range.lower;
    no_multiplier_range.upper += range.upper;
    // Not a constant multiplier, keep in dividend.
    return true;
  });

  // If we removed everything, skip the div.
  if (new_dividend == zero) {
    return extracted;
  }

  if ((d % multiplier_gcd) == 0) {
    if (no_multiplier_range.lower >= 0 &&
        no_multiplier_range.upper < multiplier_gcd) {
      // Remove everything that doesn't have a multiplier.
      new_dividend = RewriteSumIf(new_dividend, [&](AffineExpr expr) {
        auto mult = GetConstantRhs(expr, AffineExprKind::Mul);
        return mult.has_value();
      });
    }
  }

  // If we have a gcd > 1, we can split the div into two:
  // (x * 128 + y) // 192 -> (x * 2 + y // 64) // 3
  if (max_remaining_multiplier_gcd > 1) {
    AffineExpr partially_extracted = getAffineConstantExpr(0, mlir_context);
    new_dividend = RewriteSumIf(new_dividend, [&](AffineExpr expr) {
      if (auto multiplier = GetConstantRhs(expr, AffineExprKind::Mul);
          multiplier && (*multiplier > 0) &&
          ((*multiplier % max_remaining_multiplier_gcd) == 0)) {
        auto expr_lhs = mlir::cast<AffineBinaryOpExpr>(expr).getLHS();
        partially_extracted =
            partially_extracted +
            expr_lhs * (*multiplier / max_remaining_multiplier_gcd);
        // Remove from dividend.
        return false;
      }
      return true;
    });
    return extracted + (partially_extracted +
                        new_dividend.floorDiv(max_remaining_multiplier_gcd))
                           .floorDiv(d / max_remaining_multiplier_gcd);
  }

  // If we removed nothing, return the original division.
  if (extracted == getAffineConstantExpr(0, mlir_context) &&
      new_dividend == div.getLHS()) {
    return div;
  }

  return extracted + new_dividend.floorDiv(div.getRHS());
}

std::optional<int64_t> AffineExprSimplifier::GetConstantRhs(
    AffineExpr expr, AffineExprKind kind) {
  if (expr.getKind() != kind) {
    return std::nullopt;
  }
  auto bound = range_evaluator_->ComputeExpressionRange(
      mlir::cast<AffineBinaryOpExpr>(expr).getRHS());
  if (!bound.IsPoint()) {
    return std::nullopt;
  }
  return bound.lower;
}

AffineExpr AffineExprSimplifier::RewriteSum(
    AffineExpr expr, const std::function<AffineExpr(AffineExpr)>& map) {
  if (expr.getKind() == AffineExprKind::Add) {
    auto add = mlir::dyn_cast<AffineBinaryOpExpr>(expr);
    return RewriteSum(add.getLHS(), map) + RewriteSum(add.getRHS(), map);
  }
  return map(expr);
}

AffineExpr AffineExprSimplifier::RewriteSumIf(
    AffineExpr expr, const std::function<bool(AffineExpr)>& pred) {
  if (expr.getKind() == AffineExprKind::Add) {
    auto add = mlir::dyn_cast<AffineBinaryOpExpr>(expr);
    auto lhs = RewriteSumIf(add.getLHS(), pred);
    auto rhs = RewriteSumIf(add.getRHS(), pred);
    if (lhs == add.getLHS() && rhs == add.getRHS()) {
      return add;
    }
    return lhs + rhs;
  }
  return pred(expr) ? expr : mlir::getAffineConstantExpr(0, expr.getContext());
}

AffineExpr AffineExprSimplifier::SimplifyOnce(AffineExpr expr) {
  switch (expr.getKind()) {
    case AffineExprKind::Mul: {
      auto binop = mlir::cast<AffineBinaryOpExpr>(expr);
      auto lhs = SimplifyOnce(binop.getLHS());
      auto rhs = SimplifyOnce(binop.getRHS());
      return getAffineBinaryOpExpr(expr.getKind(), lhs, rhs);
    }
    case AffineExprKind::Add: {
      auto binop = mlir::cast<AffineBinaryOpExpr>(expr);
      auto lhs = SimplifyOnce(binop.getLHS());
      auto rhs = SimplifyOnce(binop.getRHS());

      // Rewrite `(x // c) * c + (x % c)` to `x`.
      // This should also work with (a+b)+c.
      auto rewrite_add = [&](AffineExpr a, AffineExpr b) -> AffineExpr {
        if (auto mod = GetConstantRhs(a, AffineExprKind::Mod)) {
          if (auto mul = GetConstantRhs(b, AffineExprKind::Mul); mod == mul) {
            auto b_lhs = mlir::cast<AffineBinaryOpExpr>(b).getLHS();
            if (auto div = GetConstantRhs(b_lhs, AffineExprKind::FloorDiv);
                div == mul) {
              auto x = mlir::cast<AffineBinaryOpExpr>(b_lhs).getLHS();
              if (x == mlir::cast<AffineBinaryOpExpr>(a).getLHS()) {
                return x;
              }
            }
          }
        }
        return nullptr;
      };

      if (auto rewritten = rewrite_add(lhs, rhs)) {
        return rewritten;
      }
      if (auto rewritten = rewrite_add(rhs, lhs)) {
        return rewritten;
      }

      return getAffineBinaryOpExpr(expr.getKind(), lhs, rhs);
    }
    case AffineExprKind::Mod:
      return RewriteMod(mlir::cast<AffineBinaryOpExpr>(expr));
    case AffineExprKind::FloorDiv:
      return RewriteFloorDiv(mlir::cast<AffineBinaryOpExpr>(expr));
    case AffineExprKind::DimId:
    case AffineExprKind::SymbolId: {
      auto bounds = range_evaluator_->ComputeExpressionRange(expr);
      if (bounds.IsPoint()) {
        return getAffineConstantExpr(bounds.lower,
                                     range_evaluator_->GetMLIRContext());
      }
      return expr;
    }

    default:
      return expr;
  }
}

AffineExpr AffineExprSimplifier::SimplifyWithMlir(AffineExpr expr, int num_dims,
                                                  int num_symbols) {
  int next_symbol = num_symbols;
  llvm::DenseMap<AffineExpr, AffineExpr> mod_to_sym;
  llvm::DenseMap<AffineExpr, AffineExpr> sym_to_mod;
  std::function<AffineExpr(AffineExpr)> replace_mods;
  replace_mods = [&](AffineExpr e) {
    switch (e.getKind()) {
      case AffineExprKind::Mul:
      case AffineExprKind::Add:
      case AffineExprKind::CeilDiv:
      case AffineExprKind::FloorDiv: {
        auto bin = mlir::cast<AffineBinaryOpExpr>(e);
        return getAffineBinaryOpExpr(e.getKind(), replace_mods(bin.getLHS()),
                                     replace_mods(bin.getRHS()));
      }
      case AffineExprKind::Mod: {
        auto& ret = mod_to_sym[e];
        if (ret) return ret;

        auto bin = mlir::cast<AffineBinaryOpExpr>(e);
        ret = getAffineSymbolExpr(next_symbol++, expr.getContext());
        sym_to_mod[ret] = getAffineBinaryOpExpr(
            AffineExprKind::Mod,
            SimplifyWithMlir(bin.getLHS(), num_dims, num_symbols),
            bin.getRHS());
        return ret;
      }
      case AffineExprKind::Constant:
      case AffineExprKind::DimId:
      case AffineExprKind::SymbolId:
        return e;
    }
  };

  auto m = replace_mods(expr);
  return mlir::simplifyAffineExpr(m, num_dims, next_symbol).replace(sym_to_mod);
}

AffineExpr AffineExprSimplifier::Simplify(AffineExpr expr) {
  while (true) {
    auto simplified = SimplifyOnce(expr);
    if (simplified == expr) {
      return expr;
    }
    expr = simplified;
  }
}

AffineMap AffineExprSimplifier::Simplify(AffineMap affine_map) {
  affine_map = SimplifyWithMlir(affine_map);
  SmallVector<AffineExpr, 4> results;
  results.reserve(affine_map.getNumResults());
  bool nothing_changed = true;
  for (AffineExpr expr : affine_map.getResults()) {
    AffineExpr simplified = Simplify(expr);
    nothing_changed &= simplified == expr;
    results.push_back(simplified);
  }
  if (nothing_changed) {
    return affine_map;
  }
  return Simplify(AffineMap::get(affine_map.getNumDims(),
                                 affine_map.getNumSymbols(), results,
                                 affine_map.getContext()));
}

// Computes intersection of two ranges.
Interval Intersect(const Interval& lhs, const Interval& rhs) {
  return Interval{std::max(lhs.lower, rhs.lower),
                  std::min(lhs.upper, rhs.upper)};
}

// Simplifies a constraint range, i.e. a constraint d0 + x in [lb, ub] will
// become d0 in [lb - x, ub - x]. Also supports *, floorDiv.
bool SimplifyConstraintRangeOnce(AffineExpr* expr, Interval* range) {
  switch (expr->getKind()) {
    case AffineExprKind::DimId:
    case AffineExprKind::SymbolId:
      // do the trick with constant
    case AffineExprKind::Constant: {
      return false;
    }
    default: {
      auto binary_op = mlir::cast<AffineBinaryOpExpr>(*expr);
      CHECK(binary_op);
      auto lhs = binary_op.getLHS();
      auto rhs = binary_op.getRHS();
      auto constant = mlir::dyn_cast<AffineConstantExpr>(rhs);
      if (!constant) {
        return false;
      }
      switch (expr->getKind()) {
        case AffineExprKind::Add: {
          int64_t shift = constant.getValue();
          range->lower -= shift;
          range->upper -= shift;
          *expr = lhs;
          return true;
        }
        case AffineExprKind::Mul: {
          int64_t factor = constant.getValue();
          if (factor < 0) {
            factor *= -1;
            range->lower *= -1;
            range->upper *= -1;
            std::swap(range->lower, range->upper);
          }
          range->lower = CeilDiv(range->lower, factor);
          range->upper = FloorDiv(range->upper, factor);
          *expr = lhs;
          return true;
        }
        case AffineExprKind::FloorDiv: {
          int64_t divisor = constant.getValue();
          if (divisor < 0) {
            divisor *= -1;
            range->lower *= -1;
            range->upper *= -1;
            std::swap(range->lower, range->upper);
          }
          range->lower *= divisor;
          range->upper = (range->upper + 1) * divisor - 1;
          *expr = lhs;
          return true;
        }
        default: {
          return false;
        }
      }
    }
  }
}

// Repeatedly simplifies the range of the constraint.
bool SimplifyConstraintRange(AffineExpr* expr, Interval* range) {
  bool is_simplified = false;
  while (SimplifyConstraintRangeOnce(expr, range)) {
    is_simplified = true;
  }
  return is_simplified;
}

}  // namespace

std::string Interval::ToString() const {
  std::stringstream ss;
  Print(ss);
  return ss.str();
}

void Interval::Print(std::ostream& out) const {
  out << '[' << lower << ", " << upper << "]";
}

std::ostream& operator<<(std::ostream& out, const Interval& range) {
  range.Print(out);
  return out;
}

bool operator==(const Interval& lhs, const Interval& rhs) {
  return lhs.lower == rhs.lower && lhs.upper == rhs.upper;
}

bool operator==(const DimVar& lhs, const DimVar& rhs) {
  return lhs.bounds == rhs.bounds;
}

bool operator==(const RangeVar& lhs, const RangeVar& rhs) {
  return lhs.range == rhs.range;
}

bool operator==(const RTVar& lhs, const RTVar& rhs) { return lhs.id == rhs.id; }

std::vector<DimVar> DimVarsFromTensorSizes(
    absl::Span<const int64_t> tensor_sizes) {
  std::vector<DimVar> ranges;
  ranges.reserve(tensor_sizes.size());
  for (int64_t size : tensor_sizes) {
    ranges.push_back({Interval{0, size - 1}});
  }
  return ranges;
}

std::vector<RangeVar> RangeVarsFromTensorSizes(
    absl::Span<const int64_t> tensor_sizes) {
  std::vector<RangeVar> ranges;
  ranges.reserve(tensor_sizes.size());
  for (int64_t size : tensor_sizes) {
    ranges.push_back({Interval{0, size - 1}});
  }
  return ranges;
}

IndexingMap IndexingMap::FromTensorSizes(
    IndexingContext* indexing_context, AffineMap affine_map,
    absl::Span<const int64_t> dim_upper_bounds,
    absl::Span<const int64_t> symbol_upper_bounds) {
  return IndexingMap{
      indexing_context, affine_map, DimVarsFromTensorSizes(dim_upper_bounds),
      RangeVarsFromTensorSizes(symbol_upper_bounds), /*rt_vars=*/{}};
}

mlir::MLIRContext* IndexingMap::GetMLIRContext() const {
  return indexing_context_->GetMLIRContext();
}

IndexingContext* IndexingMap::GetIndexingContext() const {
  return indexing_context_;
}

const Interval& IndexingMap::GetDimensionBound(int64_t dim_id) const {
  return dim_vars_[dim_id].bounds;
}

Interval& IndexingMap::GetMutableDimensionBound(int64_t dim_id) {
  return dim_vars_[dim_id].bounds;
}

std::vector<Interval> IndexingMap::GetDimensionBounds() const {
  std::vector<Interval> bounds;
  bounds.reserve(affine_map_.getNumDims());
  for (const auto& dim : dim_vars_) {
    bounds.push_back(dim.bounds);
  }
  return bounds;
}

const Interval& IndexingMap::GetSymbolBound(int64_t symbol_id) const {
  // Because affine map symbols are packed like [range_vars, rt_vars],
  // we have to pick the correct bounds.
  int64_t range_var_count = GetRangeVarsCount();
  return symbol_id < range_var_count
             ? range_vars_[symbol_id].range
             : indexing_context_
                   ->GetRTVarData(rt_vars_[symbol_id - range_var_count].id)
                   .feasible_values;
}

Interval& IndexingMap::GetMutableSymbolBound(int64_t symbol_id) {
  // Because affine map symbols are packed like [range_vars, rt_vars],
  // we have to pick the correct bounds.
  int64_t range_var_count = GetRangeVarsCount();
  return symbol_id < range_var_count
             ? range_vars_[symbol_id].range
             : indexing_context_
                   ->GetRTVarData(rt_vars_[symbol_id - range_var_count].id)
                   .feasible_values;
}

std::vector<Interval> IndexingMap::GetSymbolBounds() const {
  std::vector<Interval> bounds;
  bounds.reserve(affine_map_.getNumSymbols());
  for (const auto& range_var : range_vars_) {
    bounds.push_back(range_var.range);
  }
  for (const auto& rt_var : rt_vars_) {
    bounds.push_back(
        indexing_context_->GetRTVarData(rt_var.id).feasible_values);
  }
  return bounds;
}

void IndexingMap::AddConstraint(mlir::AffineExpr expr, Interval range) {
  if (auto dim_expr = mlir::dyn_cast<AffineDimExpr>(expr)) {
    Interval& current_range = GetMutableDimensionBound(dim_expr.getPosition());
    current_range = Intersect(current_range, range);
    return;
  }
  if (auto symbol_expr = mlir::dyn_cast<AffineSymbolExpr>(expr)) {
    Interval& current_range = GetMutableSymbolBound(symbol_expr.getPosition());
    current_range = Intersect(current_range, range);
    return;
  }
  if (SimplifyConstraintRange(&expr, &range)) {
    AddConstraint(expr, range);
    return;
  }
  auto [it, inserted] = constraints_.insert({expr, range});
  if (!inserted) {
    it->second = Intersect(it->second, range);
  }
}

bool IndexingMap::ConstraintsSatisfied(
    ArrayRef<AffineExpr> dim_const_exprs,
    ArrayRef<AffineExpr> symbol_const_exprs) const {
  CHECK(dim_const_exprs.size() == affine_map_.getNumDims());
  CHECK(symbol_const_exprs.size() == affine_map_.getNumSymbols());
  if (IsKnownEmpty()) {
    return false;
  }
  for (auto& [expr, range] : constraints_) {
    int64_t expr_value =
        mlir::cast<AffineConstantExpr>(
            expr.replaceDimsAndSymbols(dim_const_exprs, symbol_const_exprs))
            .getValue();
    if (expr_value < range.lower || expr_value > range.upper) {
      return false;
    }
  }
  return true;
}

SmallVector<int64_t, 4> IndexingMap::Evaluate(
    ArrayRef<AffineExpr> dim_const_exprs,
    ArrayRef<AffineExpr> symbol_const_exprs) const {
  CHECK(dim_const_exprs.size() == GetDimensionCount());
  CHECK(symbol_const_exprs.size() == GetSymbolCount());
  AffineMap eval = affine_map_.replaceDimsAndSymbols(
      dim_const_exprs, symbol_const_exprs, dim_const_exprs.size(),
      symbol_const_exprs.size());
  return eval.getConstantResults();
}

bool IndexingMap::IsKnownEmpty() const {
  return llvm::any_of(dim_vars_,
                      [](const DimVar& dim_var) {
                        return dim_var.bounds.lower > dim_var.bounds.upper;
                      }) ||
         llvm::any_of(range_vars_,
                      [](const RangeVar& range_var) {
                        return range_var.range.lower > range_var.range.upper;
                      }) ||
         llvm::any_of(constraints_,
                      [&](const std::pair<AffineExpr, Interval>& item) {
                        return item.second.lower > item.second.upper;
                      });
}

RangeEvaluator::RangeEvaluator(absl::Span<const Interval> dim_ranges,
                               absl::Span<const Interval> symbol_ranges,
                               MLIRContext* mlir_context)
    : mlir_context_(mlir_context) {
  for (const auto& [index, range] : llvm::enumerate(dim_ranges)) {
    expression_ranges_cache_[getAffineDimExpr(index, mlir_context_)] = range;
  }
  for (const auto& [index, range] : llvm::enumerate(symbol_ranges)) {
    expression_ranges_cache_[getAffineSymbolExpr(index, mlir_context_)] = range;
  }
}

bool RangeEvaluator::IsAlwaysPositiveOrZero(mlir::AffineExpr expr) {
  return ComputeExpressionRange(expr).lower >= 0;
}

bool RangeEvaluator::IsAlwaysNegativeOrZero(mlir::AffineExpr expr) {
  return ComputeExpressionRange(expr).upper <= 0;
}

Interval RangeEvaluator::ComputeExpressionRange(AffineExpr expr) {
  switch (expr.getKind()) {
    case AffineExprKind::Constant: {
      int64_t value = mlir::cast<AffineConstantExpr>(expr).getValue();
      return Interval{value, value};
    }
    case AffineExprKind::DimId: {
      return expression_ranges_cache_[expr];
    }
    case AffineExprKind::SymbolId: {
      return expression_ranges_cache_[expr];
    }
    default:
      auto bound = expression_ranges_cache_.find(expr);
      if (bound != expression_ranges_cache_.end()) {
        return bound->second;
      }
      auto binary_op = mlir::dyn_cast<AffineBinaryOpExpr>(expr);
      CHECK(binary_op);
      auto lhs = ComputeExpressionRange(binary_op.getLHS());
      auto rhs = ComputeExpressionRange(binary_op.getRHS());

      auto& result = expression_ranges_cache_[expr];
      switch (expr.getKind()) {
        case AffineExprKind::Add:
          return result = {lhs.lower + rhs.lower, lhs.upper + rhs.upper};
        case AffineExprKind::Mul: {
          int64_t a = lhs.lower * rhs.lower;
          int64_t b = lhs.upper * rhs.upper;
          return result = {std::min(a, b), std::max(a, b)};
        }
        case AffineExprKind::Mod: {
          CHECK(rhs.IsPoint()) << "RHS of mod must be a constant";
          int64_t m = rhs.lower;
          if (0 <= lhs.lower && lhs.upper < m) {
            return result = lhs;
          }
          return result = {0, m - 1};
        }
        case AffineExprKind::FloorDiv: {
          CHECK(rhs.IsPoint()) << "RHS of floor_div must be a constant";
          int64_t d = rhs.lower;
          int64_t a = FloorDiv(lhs.lower, d);
          int64_t b = FloorDiv(lhs.upper, d);
          return result = {std::min(a, b), std::max(a, b)};
        }
        default:
          // We don't use ceildiv, so we don't support it.
          LOG(FATAL) << "Unsupported expression";
      }
  }
}

std::string IndexingMap::ToString(const AffineMapPrinter& printer) const {
  std::stringstream ss;
  Print(ss, printer);
  return ss.str();
}

void IndexingMap::Print(std::ostream& out,
                        const AffineMapPrinter& printer) const {
  printer.Print(out, affine_map_);
  out << "\ndomain:\n";
  for (const auto& [index, range] : llvm::enumerate(dim_vars_)) {
    out << printer.GetDimensionName(static_cast<int64_t>(index)) << " in ";
    dim_vars_.at(index).bounds.Print(out);
    out << '\n';
  }
  int64_t range_vars_count = GetRangeVarsCount();
  for (const auto& [index, range] : llvm::enumerate(range_vars_)) {
    out << printer.GetSymbolName(static_cast<int64_t>(index)) << " in ";
    range_vars_.at(index).range.Print(out);
    out << '\n';
  }
  for (const auto& [index, range] : llvm::enumerate(rt_vars_)) {
    auto id = rt_vars_.at(index).id;
    const RTVarData& rt_var_data = indexing_context_->GetRTVarData(id);
    out << printer.GetSymbolName(static_cast<int64_t>(range_vars_count + index))
        << " id: " << id;
    rt_var_data.Print(out);
  }
  std::vector<std::string> expr_range_strings;
  expr_range_strings.reserve(constraints_.size());
  for (const auto& [expr, range] : constraints_) {
    std::stringstream ss;
    printer.Print(ss, expr);
    ss << " in ";
    range.Print(ss);
    expr_range_strings.push_back(ss.str());
  }
  std::sort(expr_range_strings.begin(), expr_range_strings.end());
  for (const auto& expr_range_string : expr_range_strings) {
    out << expr_range_string << '\n';
  }
}

std::ostream& operator<<(std::ostream& out, const IndexingMap& indexing_map) {
  AffineMapPrinter printer;
  indexing_map.Print(out, printer);
  return out;
}

bool operator==(const IndexingMap& lhs, const IndexingMap& rhs) {
  return lhs.GetAffineMap() == rhs.GetAffineMap() &&
         lhs.GetDimVars() == rhs.GetDimVars() &&
         lhs.GetRangeVars() == rhs.GetRangeVars() &&
         lhs.GetRTVars() == rhs.GetRTVars();
}

IndexingMap operator*(const IndexingMap& lhs, const IndexingMap& rhs) {
  return ComposeIndexingMaps(lhs, rhs);
}

// Simplification of IndexingMap has two main parts.
// At first we optimized constraints to make the domain as small and simple as
// possible. And only then we simplify the affine_map, because its
// simplification relies on lower/upper bounds of dimensions and symbols.

// Constraint simplification is performed in two stages repeated until
// convergence.
//   1. Simplify affine expressions in all constraints.
//   2. Simplify constraint ranges for all constraints.
// We don't optimize every constraint separately to avoid re-initialization of
// RangeEvaluator for every constraint. Note that we start with "expr"
// simplification, because the ranges of constraints were already optimized once
// when IndexingMap was constructed.
bool IndexingMap::Simplify() {
  if (IsUndefined()) return false;

  // Simplify constraints to shrink the lower/upper bounds of dims and symbols.
  bool constraints_were_simplified = false;
  while (true) {
    if (!SimplifyConstraintExprs()) break;
    constraints_were_simplified = true;
    if (!SimplifyConstraintRanges()) break;
  }
  // Simplify affine_map using the optimized ranges.
  // Potentially, we can be smarter about recreating the range_evaluator.
  RangeEvaluator range_evaluator(GetDimensionBounds(), GetSymbolBounds(),
                                 GetMLIRContext());
  AffineMap simplified_affine_map =
      AffineExprSimplifier(&range_evaluator).Simplify(affine_map_);
  bool affine_map_was_simplified = simplified_affine_map != affine_map_;
  if (affine_map_was_simplified) {
    affine_map_ = simplified_affine_map;
  }
  return affine_map_was_simplified || constraints_were_simplified;
}

bool IndexingMap::SimplifyConstraintExprs() {
  // Simplify affine expression in the constraints_.
  RangeEvaluator range_evaluator(GetDimensionBounds(), GetSymbolBounds(),
                                 GetMLIRContext());
  AffineExprSimplifier simplifier(&range_evaluator);
  std::vector<AffineExpr> to_remove;
  std::vector<std::pair<AffineExpr, Interval>> to_add;
  for (const auto& [expr, range] : constraints_) {
    AffineExpr simplified = simplifier.Simplify(expr);

    // Skip constraints that are always satisfied.
    Interval evaluated_range =
        range_evaluator.ComputeExpressionRange(simplified);
    if (evaluated_range.upper <= range.upper &&
        evaluated_range.lower >= range.lower) {
      to_remove.push_back(expr);
      continue;
    }
    if (simplified == expr) continue;
    to_add.push_back({simplified, range});
    to_remove.push_back(expr);
  }
  for (const auto& expr : to_remove) {
    constraints_.erase(expr);
  }
  for (const auto& [expr, range] : to_add) {
    AddConstraint(expr, range);
  }
  return !to_add.empty();
}

bool IndexingMap::SimplifyConstraintRanges() {
  std::vector<AffineExpr> to_remove;
  std::vector<std::pair<AffineExpr, Interval>> to_add;
  for (const auto& [expr, range] : constraints_) {
    AffineExpr simplified_expr = expr;
    Interval simplified_range = range;
    if (SimplifyConstraintRange(&simplified_expr, &simplified_range)) {
      to_add.push_back({simplified_expr, simplified_range});
      to_remove.push_back(expr);
    }
  }
  for (const auto& expr : to_remove) {
    constraints_.erase(expr);
  }
  for (const auto& [expr, range] : to_add) {
    AddConstraint(expr, range);
  }
  return !to_add.empty();
}

namespace {

struct UsedParameters {
  llvm::DenseSet<int64_t> dimension_ids;
  llvm::DenseSet<int64_t> symbol_ids;
};

void GetUsedParametersImpl(const AffineExpr& expr,
                           UsedParameters& used_parameters) {
  if (auto dim_expr = mlir::dyn_cast<AffineDimExpr>(expr)) {
    used_parameters.dimension_ids.insert(dim_expr.getPosition());
    return;
  }
  if (auto symbol_expr = mlir::dyn_cast<AffineSymbolExpr>(expr)) {
    used_parameters.symbol_ids.insert(symbol_expr.getPosition());
    return;
  }
  if (auto binary_expr = mlir::dyn_cast<AffineBinaryOpExpr>(expr)) {
    GetUsedParametersImpl(binary_expr.getLHS(), used_parameters);
    GetUsedParametersImpl(binary_expr.getRHS(), used_parameters);
  }
}

// Returns IDs of dimensions and symbols that participate in AffineExpr.
UsedParameters GetUsedParameters(const mlir::AffineExpr& expr) {
  UsedParameters used_parameters;
  GetUsedParametersImpl(expr, used_parameters);
  return used_parameters;
}

bool IsFunctionOfUnusedDimsAndSymbolsOnly(
    const UsedParameters& used_parameters,
    const SmallBitVector& unused_dims_bit_vector,
    const SmallBitVector& unused_symbols_bit_vector) {
  for (int64_t dim_id : used_parameters.dimension_ids) {
    if (!unused_dims_bit_vector[dim_id]) return false;
  }
  for (int64_t symbol_id : used_parameters.symbol_ids) {
    if (!unused_symbols_bit_vector[symbol_id]) return false;
  }
  return true;
}

}  // namespace

void IndexingMap::RemoveUnusedSymbols() {
  if (IsUndefined()) return;
  // TODO(b/329052892): Implement composition with RT vars.
  if (GetRTVarsCount()) return;

  // Remove unused symbols from the affine_map.
  unsigned num_symbols_before = affine_map_.getNumSymbols();
  SmallBitVector unused_symbols_bit_vector =
      mlir::getUnusedSymbolsBitVector({affine_map_});
  SmallBitVector unused_dims_bit_vector =
      mlir::getUnusedDimsBitVector({affine_map_});

  // Check if the symbols that are unused in `affine_map` are also unused in
  // expressions.
  std::vector<std::pair<AffineExpr, UsedParameters>> candidates_to_remove;
  for (const auto& [expr, range] : constraints_) {
    UsedParameters used_parameters = GetUsedParameters(expr);
    // If the expression uses only symbols and dims that are "unused" in
    // `affine_map`, then we can remove it.
    if (IsFunctionOfUnusedDimsAndSymbolsOnly(used_parameters,
                                             unused_dims_bit_vector,
                                             unused_symbols_bit_vector)) {
      candidates_to_remove.push_back({expr, used_parameters});
      continue;
    }
    // Otherwise, we need to mark all symbols of these expr as "used".
    for (int64_t symbol_id : used_parameters.symbol_ids) {
      unused_symbols_bit_vector[symbol_id] = false;
    }
  }
  for (const auto& [expr, used_parameters] : candidates_to_remove) {
    if (IsFunctionOfUnusedDimsAndSymbolsOnly(used_parameters,
                                             unused_dims_bit_vector,
                                             unused_symbols_bit_vector)) {
      constraints_.erase(expr);
    }
  }

  // Compress `affine_map` using the updated `unused_symbols_bit_vector`.
  affine_map_ = mlir::compressSymbols(affine_map_, unused_symbols_bit_vector);

  // Remap symbols in the constraint expressions accordingly.
  unsigned num_symbols_after = affine_map_.getNumSymbols();
  if (num_symbols_after == num_symbols_before) return;

  std::vector<RangeVar> compressed_range_vars;
  MLIRContext* mlir_context = GetMLIRContext();
  int64_t used_symbols_count = 0;
  std::vector<AffineExpr> symbol_replacements(
      num_symbols_before, getAffineConstantExpr(0, mlir_context));
  for (int i = 0; i < unused_symbols_bit_vector.size(); ++i) {
    if (!unused_symbols_bit_vector[i]) {
      compressed_range_vars.push_back(range_vars_[i]);
      symbol_replacements[i] =
          getAffineSymbolExpr(used_symbols_count++, mlir_context);
    }
  }
  range_vars_ = std::move(compressed_range_vars);
  std::vector<AffineExpr> to_remove;
  std::vector<std::pair<AffineExpr, Interval>> to_add;
  for (const auto& [expr, range] : constraints_) {
    auto updated_expr = expr.replaceSymbols(symbol_replacements);
    if (updated_expr == expr) continue;
    to_add.push_back({updated_expr, range});
    to_remove.push_back(expr);
  }
  for (const auto& expr : to_remove) {
    constraints_.erase(expr);
  }
  for (const auto& [expr, range] : to_add) {
    AddConstraint(expr, range);
  }
}

IndexingMap ComposeIndexingMaps(const IndexingMap& first,
                                const IndexingMap& second) {
  if (second.IsUndefined() || first.IsUndefined()) {
    return IndexingMap::GetUndefined();
  }
  // TODO(b/329052892): Implement composition with RT vars.
  if (first.GetRTVarsCount() || second.GetRTVarsCount()) {
    return IndexingMap::GetUndefined();
  }
  AffineMap producer_affine_map = second.GetAffineMap();
  AffineMap composed_map = producer_affine_map.compose(first.GetAffineMap());

  // The symbols in the composed map, i.e. combined
  // producer_map.compose(consumer_map) are packed as [symbols(producer_map) |
  // symbols(consumer_map)].
  std::vector<RangeVar> combined_symbol_ranges;
  combined_symbol_ranges.reserve(second.GetRangeVarsCount() +
                                 first.GetRangeVarsCount());
  for (const RangeVar& symbol_range : llvm::concat<const RangeVar>(
           second.GetRangeVars(), first.GetRangeVars())) {
    combined_symbol_ranges.push_back(symbol_range);
  }

  IndexingContext* indexing_context = first.GetIndexingContext();
  IndexingMap composed_indexing_map(indexing_context, composed_map,
                                    first.GetDimVars(),
                                    std::move(combined_symbol_ranges),
                                    /*rt_vars=*/{});
  // Add constraints that are already present in the producer_map. We have to
  // compute consumer_map(producer_constraints). To keep all symbols and
  // dimension IDs the same as in the `composed_indexing_map.affine_map`, we
  // create an AffineMap
  // (dims of producer_affine_map)[symbols_of_producer_affine_map] =
  //   (constraint_1, ..., constraint_N) and then compose.
  std::vector<AffineExpr> constraints;
  std::vector<Interval> constraints_ranges;
  for (const auto& [expr, range] : second.GetConstraints()) {
    constraints.push_back(expr);
    constraints_ranges.push_back(range);
  }
  auto constraints_map = AffineMap::get(
      producer_affine_map.getNumDims(), producer_affine_map.getNumSymbols(),
      constraints, producer_affine_map.getContext());
  auto remapped_constraints = constraints_map.compose(first.GetAffineMap());
  for (const auto& [expr, range] :
       llvm::zip(remapped_constraints.getResults(), constraints_ranges)) {
    composed_indexing_map.AddConstraint(expr, range);
  }
  // Remap symbol ids and add constraints that are already present in the
  // consumer_map.
  for (const auto& [expr, range] : first.GetConstraints()) {
    composed_indexing_map.AddConstraint(
        expr.shiftSymbols(first.GetSymbolCount(), second.GetSymbolCount()),
        range);
  }
  // Add constraints for consumer's codomain w.r.t. producer's domain.
  for (auto [index, expr] :
       llvm::enumerate(first.GetAffineMap().getResults())) {
    Interval producer_dim_range =
        second.GetDimensionBound(static_cast<int64_t>(index));
    composed_indexing_map.AddConstraint(
        expr.shiftSymbols(first.GetSymbolCount(), second.GetSymbolCount()),
        producer_dim_range);
  }
  return composed_indexing_map;
}

std::string RTVarData::ToString() const {
  std::stringstream ss;
  Print(ss);
  return ss.str();
}

void RTVarData::Print(std::ostream& out) const {
  out << " in " << feasible_values
      << "\nhlo: " << (hlo == nullptr ? "NULL" : hlo->ToString()) << '\n';
  indexing_map.Print(out, AffineMapPrinter());
}

}  // namespace gpu
}  // namespace xla
