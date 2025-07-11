/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                       */
/*    This file is part of the HiGHS linear optimization suite           */
/*                                                                       */
/*    Available as open-source under the MIT License                     */
/*                                                                       */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/**@file lp_data/HighsInterface.cpp
 * @brief
 */
#include <sstream>

#include "Highs.h"
#include "lp_data/HighsLpUtils.h"
#include "lp_data/HighsModelUtils.h"
#include "model/HighsHessianUtils.h"
#include "simplex/HSimplex.h"
#include "util/HighsMatrixUtils.h"
#include "util/HighsSort.h"

void Highs::reportModelStats() const {
  const HighsLp& lp = this->model_.lp_;
  const HighsHessian& hessian = this->model_.hessian_;
  const HighsLogOptions& log_options = this->options_.log_options;
  if (!*log_options.output_flag) return;
  HighsInt num_integer = 0;
  HighsInt num_binary = 0;
  HighsInt num_semi_continuous = 0;
  HighsInt num_semi_integer = 0;
  for (HighsInt iCol = 0; iCol < static_cast<HighsInt>(lp.integrality_.size());
       iCol++) {
    switch (lp.integrality_[iCol]) {
      case HighsVarType::kInteger:
        num_integer++;
        if (lp.col_lower_[iCol] == 0 && lp.col_upper_[iCol] == 1) num_binary++;
        break;
      case HighsVarType::kSemiContinuous:
        num_semi_continuous++;
        break;
      case HighsVarType::kSemiInteger:
        num_semi_integer++;
        break;
      default:
        break;
    }
  }
  std::string problem_type;
  const bool non_continuous =
      num_integer + num_semi_continuous + num_semi_integer;
  if (hessian.dim_) {
    if (non_continuous) {
      problem_type = "MIQP";
    } else {
      problem_type = "QP  ";
    }
  } else {
    if (non_continuous) {
      problem_type = "MIP ";
    } else {
      problem_type = "LP  ";
    }
  }
  const HighsInt a_num_nz = lp.a_matrix_.numNz();
  const HighsInt q_num_nz = hessian.dim_ > 0 ? hessian.numNz() : 0;
  if (*log_options.log_dev_level) {
    highsLogDev(log_options, HighsLogType::kInfo, "%4s      : %s\n",
                problem_type.c_str(), lp.model_name_.c_str());
    highsLogDev(log_options, HighsLogType::kInfo,
                "Rows      : %" HIGHSINT_FORMAT "\n", lp.num_row_);
    highsLogDev(log_options, HighsLogType::kInfo,
                "Cols      : %" HIGHSINT_FORMAT "\n", lp.num_col_);
    if (q_num_nz) {
      highsLogDev(log_options, HighsLogType::kInfo,
                  "Matrix Nz : %" HIGHSINT_FORMAT "\n", a_num_nz);
      highsLogDev(log_options, HighsLogType::kInfo,
                  "Hessian Nz: %" HIGHSINT_FORMAT "\n", q_num_nz);
    } else {
      highsLogDev(log_options, HighsLogType::kInfo,
                  "Nonzeros  : %" HIGHSINT_FORMAT "\n", a_num_nz);
    }
    if (num_integer)
      highsLogDev(log_options, HighsLogType::kInfo,
                  "Integer   : %" HIGHSINT_FORMAT " (%" HIGHSINT_FORMAT
                  " binary)\n",
                  num_integer, num_binary);
    if (num_semi_continuous)
      highsLogDev(log_options, HighsLogType::kInfo,
                  "SemiConts : %" HIGHSINT_FORMAT "\n", num_semi_continuous);
    if (num_semi_integer)
      highsLogDev(log_options, HighsLogType::kInfo,
                  "SemiInt   : %" HIGHSINT_FORMAT "\n", num_semi_integer);
  } else {
    std::stringstream stats_line;
    stats_line << problem_type;
    if (lp.model_name_.length()) stats_line << " " << lp.model_name_;
    stats_line << " has " << lp.num_row_ << " rows; " << lp.num_col_ << " cols";
    if (q_num_nz) {
      stats_line << "; " << a_num_nz << " matrix nonzeros";
      stats_line << "; " << q_num_nz << " Hessian nonzeros";
    } else {
      stats_line << "; " << a_num_nz << " nonzeros";
    }
    if (num_integer)
      stats_line << "; " << num_integer << " integer variables (" << num_binary
                 << " binary)";
    if (num_semi_continuous)
      stats_line << "; " << num_semi_continuous << " semi-continuous variables";
    if (num_semi_integer)
      stats_line << "; " << num_semi_integer << " semi-integer variables";
    highsLogUser(log_options, HighsLogType::kInfo, "%s\n",
                 stats_line.str().c_str());
  }
}

HighsStatus Highs::formStandardFormLp() {
  this->clearStandardFormLp();
  HighsLp& lp = this->model_.lp_;
  HighsSparseMatrix& matrix = lp.a_matrix_;
  // Ensure that the incumbent LP and standard form LP matrices are rowwise
  matrix.ensureRowwise();
  // Original rows are processed before columns, so that any original
  // boxed rows can be transformed to pairs of one-sided rows,
  // requiring the standard form matrix to be row-wise. The original
  // columns are assumed to come before any new columns, so their
  // costs (as a minimization) must be defined befor costs of new
  // columns.
  // Determine the objective scaling, and apply it to any offset
  HighsInt sense = HighsInt(lp.sense_);
  this->standard_form_offset_ = sense * lp.offset_;
  for (HighsInt iCol = 0; iCol < lp.num_col_; iCol++)
    this->standard_form_cost_.push_back(sense * lp.col_cost_[iCol]);
  this->standard_form_matrix_.format_ = MatrixFormat::kRowwise;
  this->standard_form_matrix_.num_col_ = lp.num_col_;
  // Create a HighsSparseMatrix instance to store rows extracted from
  // the original constraint matrix
  HighsInt local_row_min_nnz = std::max(lp.num_col_, HighsInt(2));
  HighsSparseMatrix local_row;
  local_row.ensureRowwise();
  local_row.num_row_ = 1;
  local_row.num_col_ = lp.num_col_;
  local_row.index_.resize(local_row_min_nnz);
  local_row.value_.resize(local_row_min_nnz);
  local_row.start_.resize(2);
  HighsInt& num_nz = local_row.start_[1];
  local_row.start_[0] = 0;
  HighsInt num_fixed_row = 0;
  HighsInt num_boxed_row = 0;
  HighsInt num_lower_row = 0;
  HighsInt num_upper_row = 0;
  HighsInt num_free_row = 0;
  HighsInt num_fixed_col = 0;
  HighsInt num_boxed_col = 0;
  HighsInt num_lower_col = 0;
  HighsInt num_upper_col = 0;
  HighsInt num_free_col = 0;
  std::vector<HighsInt> slack_ix;
  for (HighsInt iRow = 0; iRow < lp.num_row_; iRow++) {
    double lower = lp.row_lower_[iRow];
    double upper = lp.row_upper_[iRow];
    if (lower <= -kHighsInf && upper >= kHighsInf) {
      assert(0 == 1);
      // Free row
      num_free_row++;
      continue;
    }
    if (lower == upper) {
      // Equality row
      num_fixed_row++;
      matrix.getRow(iRow, num_nz, local_row.index_.data(),
                    local_row.value_.data());
      this->standard_form_matrix_.addRows(local_row);
      this->standard_form_rhs_.push_back(upper);
      continue;
    } else if (lower <= -kHighsInf) {
      // Upper bounded row, so record the slack
      num_upper_row++;
      assert(upper < kHighsInf);
      HighsInt standard_form_row = this->standard_form_rhs_.size();
      slack_ix.push_back(standard_form_row + 1);
      matrix.getRow(iRow, num_nz, local_row.index_.data(),
                    local_row.value_.data());
      this->standard_form_matrix_.addRows(local_row);
      this->standard_form_rhs_.push_back(upper);
    } else if (upper >= kHighsInf) {
      // Lower bounded row, so record the slack
      num_lower_row++;
      assert(lower > -kHighsInf);
      HighsInt standard_form_row = this->standard_form_rhs_.size();
      slack_ix.push_back(-(standard_form_row + 1));
      matrix.getRow(iRow, num_nz, local_row.index_.data(),
                    local_row.value_.data());
      this->standard_form_matrix_.addRows(local_row);
      this->standard_form_rhs_.push_back(lower);
    } else {
      // Boxed row, so record the lower slack
      assert(lower > -kHighsInf);
      assert(upper < kHighsInf);
      num_boxed_row++;
      HighsInt standard_form_row = this->standard_form_rhs_.size();
      slack_ix.push_back(-(standard_form_row + 1));
      matrix.getRow(iRow, num_nz, local_row.index_.data(),
                    local_row.value_.data());
      this->standard_form_matrix_.addRows(local_row);
      this->standard_form_rhs_.push_back(lower);
      // .. and upper slack, adding a copy of the row
      standard_form_row = this->standard_form_rhs_.size();
      slack_ix.push_back(standard_form_row + 1);
      this->standard_form_matrix_.addRows(local_row);
      this->standard_form_rhs_.push_back(upper);
    }
  }
  // Add rows corresponding to boxed columns
  for (HighsInt iCol = 0; iCol < lp.num_col_; iCol++) {
    double lower = lp.col_lower_[iCol];
    double upper = lp.col_upper_[iCol];
    if (lower > -kHighsInf && upper < kHighsInf) {
      // Boxed column
      //
      // x will be replaced by x = l + X (below) with X >= 0
      //
      // Introduce variable s >= 0 so that (with x >= l still)
      //
      // x = u - s => x + s = u
      this->standard_form_cost_.push_back(0);
      this->standard_form_matrix_.num_col_++;
      local_row.num_col_++;
      local_row.index_[0] = iCol;
      local_row.index_[1] = this->standard_form_matrix_.num_col_ - 1;
      local_row.value_[0] = 1;
      local_row.value_[1] = 1;
      local_row.start_[1] = 2;
      this->standard_form_matrix_.addRows(local_row);
      this->standard_form_rhs_.push_back(upper);
    }
  }
  // Finished with both matrices, row-wise, so ensure that the
  // incumbent matrix leaves col-wise, and that the standard form
  // matrix is col-wise so RHS shifts can be applied and more columns
  // can be added
  matrix.ensureColwise();
  this->standard_form_matrix_.ensureColwise();
  // Work through the columns, ensuring that all have non-negativity bounds
  for (HighsInt iCol = 0; iCol < lp.num_col_; iCol++) {
    double cost = sense * lp.col_cost_[iCol];
    double lower = lp.col_lower_[iCol];
    double upper = lp.col_upper_[iCol];
    if (lower > -kHighsInf) {
      // Finite lower bound
      if (upper < kHighsInf) {
        if (lower == upper) {
          // Fixed column
          num_fixed_col++;
        } else {
          // Boxed column
          num_boxed_col++;
        }
      } else {
        // Lower column
        num_lower_col++;
      }
      if (lower != 0) {
        // x >= l, so shift x-l = X >= 0, giving x = X + l
        //
        // Cost contribution c(X+l) = cX + cl
        this->standard_form_offset_ += cost * lower;
        // Constraint contribution a(X+l) = aX + al
        for (HighsInt iEl = this->standard_form_matrix_.start_[iCol];
             iEl < this->standard_form_matrix_.start_[iCol + 1]; iEl++)
          this->standard_form_rhs_[this->standard_form_matrix_.index_[iEl]] -=
              this->standard_form_matrix_.value_[iEl] * lower;
      }
    } else if (upper < kHighsInf) {
      // Upper column
      num_upper_col++;
      // Have to operate even if u=0, since cost and column values are negated
      //
      // x <= u, so shift u-x = X >= 0, giving x = u - X
      //
      // Cost contribution c(u-X) = cu - cX
      this->standard_form_offset_ += cost * upper;
      this->standard_form_cost_[iCol] = -cost;
      // Constraint contribution a(u-X) = -aX + au
      for (HighsInt iEl = this->standard_form_matrix_.start_[iCol];
           iEl < this->standard_form_matrix_.start_[iCol + 1]; iEl++) {
        this->standard_form_rhs_[this->standard_form_matrix_.index_[iEl]] -=
            this->standard_form_matrix_.value_[iEl] * upper;
        this->standard_form_matrix_.value_[iEl] =
            -this->standard_form_matrix_.value_[iEl];
      }
    } else {
      // Free column
      num_free_col++;
      // Represent as x = x+ - x-
      //
      // where original column is now x+ >= 0
      //
      // and x- >= 0 has negation of its cost and matrix column
      this->standard_form_cost_.push_back(-cost);
      for (HighsInt iEl = this->standard_form_matrix_.start_[iCol];
           iEl < this->standard_form_matrix_.start_[iCol + 1]; iEl++) {
        this->standard_form_matrix_.index_.push_back(
            this->standard_form_matrix_.index_[iEl]);
        this->standard_form_matrix_.value_.push_back(
            -this->standard_form_matrix_.value_[iEl]);
      }
      this->standard_form_matrix_.start_.push_back(
          HighsInt(this->standard_form_matrix_.index_.size()));
    }
  }
  // Now add the slack variables
  for (HighsInt iRow : slack_ix) {
    this->standard_form_cost_.push_back(0);
    if (iRow > 0) {
      this->standard_form_matrix_.index_.push_back(iRow - 1);
      this->standard_form_matrix_.value_.push_back(1);
    } else {
      this->standard_form_matrix_.index_.push_back(-iRow - 1);
      this->standard_form_matrix_.value_.push_back(-1);
    }
    this->standard_form_matrix_.start_.push_back(
        HighsInt(this->standard_form_matrix_.index_.size()));
  }
  // Now set correct values for the dimensions of this->standard_form_matrix_
  this->standard_form_matrix_.num_col_ = int(standard_form_cost_.size());
  this->standard_form_matrix_.num_row_ = int(standard_form_rhs_.size());
  this->standard_form_valid_ = true;
  highsLogUser(options_.log_options, HighsLogType::kInfo,
               "Standard form LP obtained for LP with (free / lower / upper / "
               "boxed / fixed) variables"
               " (%d / %d / %d / %d / %d) and constraints"
               " (%d / %d / %d / %d / %d) \n",
               int(num_free_col), int(num_lower_col), int(num_upper_col),
               int(num_boxed_col), int(num_fixed_col), int(num_free_row),
               int(num_lower_row), int(num_upper_row), int(num_boxed_row),
               int(num_fixed_row));
  return HighsStatus::kOk;
}

HighsStatus Highs::basisForSolution() {
  HighsLp& lp = model_.lp_;
  assert(!lp.isMip() || options_.solve_relaxation);
  assert(solution_.value_valid);
  invalidateBasis();
  HighsInt num_basic = 0;
  HighsBasis basis;
  for (HighsInt iCol = 0; iCol < lp.num_col_; iCol++) {
    if (std::fabs(lp.col_lower_[iCol] - solution_.col_value[iCol]) <=
        options_.primal_feasibility_tolerance) {
      basis.col_status.push_back(HighsBasisStatus::kLower);
    } else if (std::fabs(lp.col_upper_[iCol] - solution_.col_value[iCol]) <=
               options_.primal_feasibility_tolerance) {
      basis.col_status.push_back(HighsBasisStatus::kUpper);
    } else {
      num_basic++;
      basis.col_status.push_back(HighsBasisStatus::kBasic);
    }
  }
  const HighsInt num_basic_col = num_basic;
  for (HighsInt iRow = 0; iRow < lp.num_row_; iRow++) {
    if (std::fabs(lp.row_lower_[iRow] - solution_.row_value[iRow]) <=
        options_.primal_feasibility_tolerance) {
      basis.row_status.push_back(HighsBasisStatus::kLower);
    } else if (std::fabs(lp.row_upper_[iRow] - solution_.row_value[iRow]) <=
               options_.primal_feasibility_tolerance) {
      basis.row_status.push_back(HighsBasisStatus::kUpper);
    } else {
      num_basic++;
      basis.row_status.push_back(HighsBasisStatus::kBasic);
    }
  }
  const HighsInt num_basic_row = num_basic - num_basic_col;
  assert((int)basis.col_status.size() == lp.num_col_);
  assert((int)basis.row_status.size() == lp.num_row_);
  highsLogDev(options_.log_options, HighsLogType::kInfo,
              "LP has %d rows and solution yields %d possible basic variables "
              "(%d / %d; %d / %d)\n",
              (int)lp.num_row_, (int)num_basic, (int)num_basic_col,
              (int)lp.num_col_, (int)num_basic_row, (int)lp.num_row_);
  return this->setBasis(basis);
}

HighsStatus Highs::addColsInterface(
    HighsInt ext_num_new_col, const double* ext_col_cost,
    const double* ext_col_lower, const double* ext_col_upper,
    HighsInt ext_num_new_nz, const HighsInt* ext_a_start,
    const HighsInt* ext_a_index, const double* ext_a_value) {
  HighsStatus return_status = HighsStatus::kOk;
  HighsOptions& options = options_;
  if (ext_num_new_col < 0) return HighsStatus::kError;
  if (ext_num_new_nz < 0) return HighsStatus::kError;
  if (ext_num_new_col == 0) return HighsStatus::kOk;
  if (ext_num_new_col > 0)
    if (isColDataNull(options.log_options, ext_col_cost, ext_col_lower,
                      ext_col_upper))
      return HighsStatus::kError;
  if (ext_num_new_nz > 0)
    if (isMatrixDataNull(options.log_options, ext_a_start, ext_a_index,
                         ext_a_value))
      return HighsStatus::kError;

  HighsLp& lp = model_.lp_;
  HighsBasis& basis = basis_;
  HighsScale& scale = lp.scale_;
  bool& useful_basis = basis.useful;
  bool& lp_has_scaling = lp.scale_.has_scaling;

  // Check that if nonzeros are to be added then the model has a positive number
  // of rows
  if (lp.num_row_ <= 0 && ext_num_new_nz > 0) return HighsStatus::kError;

  // Record the new number of columns
  HighsInt newNumCol = lp.num_col_ + ext_num_new_col;

  HighsIndexCollection index_collection;
  index_collection.dimension_ = ext_num_new_col;
  index_collection.is_interval_ = true;
  index_collection.from_ = 0;
  index_collection.to_ = ext_num_new_col - 1;

  // Take a copy of the cost and bounds that can be normalised
  std::vector<double> local_colCost{ext_col_cost,
                                    ext_col_cost + ext_num_new_col};
  std::vector<double> local_colLower{ext_col_lower,
                                     ext_col_lower + ext_num_new_col};
  std::vector<double> local_colUpper{ext_col_upper,
                                     ext_col_upper + ext_num_new_col};

  bool local_has_infinite_cost = false;
  return_status = interpretCallStatus(
      options_.log_options,
      assessCosts(options, lp.num_col_, index_collection, local_colCost,
                  local_has_infinite_cost, options.infinite_cost),
      return_status, "assessCosts");
  if (return_status == HighsStatus::kError) return return_status;
  // Assess the column bounds
  return_status = interpretCallStatus(
      options_.log_options,
      assessBounds(options, "Col", lp.num_col_, index_collection,
                   local_colLower, local_colUpper, options.infinite_bound),
      return_status, "assessBounds");
  if (return_status == HighsStatus::kError) return return_status;
  if (lp.user_bound_scale_) {
    // Assess and apply any user bound scaling
    if (!boundScaleOk(local_colLower, local_colUpper, lp.user_bound_scale_,
                      options.infinite_bound)) {
      highsLogUser(options_.log_options, HighsLogType::kError,
                   "User bound scaling yields infinite bound\n");
      return HighsStatus::kError;
    }
    double bound_scale_value = std::pow(2, lp.user_bound_scale_);
    for (HighsInt iCol = 0; iCol < ext_num_new_col; iCol++) {
      local_colLower[iCol] *= bound_scale_value;
      local_colUpper[iCol] *= bound_scale_value;
    }
  }
  if (lp.user_cost_scale_) {
    // Assess and apply any user cost scaling
    if (!costScaleOk(local_colCost, lp.user_cost_scale_,
                     options.infinite_cost)) {
      highsLogUser(options_.log_options, HighsLogType::kError,
                   "User cost scaling yields infinite cost\n");
      return HighsStatus::kError;
    }
    double cost_scale_value = std::pow(2, lp.user_cost_scale_);
    for (HighsInt iCol = 0; iCol < ext_num_new_col; iCol++)
      local_colCost[iCol] *= cost_scale_value;
  }
  // Append the columns to the LP vectors and matrix
  appendColsToLpVectors(lp, ext_num_new_col, local_colCost, local_colLower,
                        local_colUpper);
  // Form a column-wise HighsSparseMatrix of the new matrix columns so
  // that is easy to handle and, if there are nonzeros, it can be
  // normalised
  HighsSparseMatrix local_a_matrix;
  local_a_matrix.num_col_ = ext_num_new_col;
  local_a_matrix.num_row_ = lp.num_row_;
  local_a_matrix.format_ = MatrixFormat::kColwise;
  if (ext_num_new_nz) {
    local_a_matrix.start_ = {ext_a_start, ext_a_start + ext_num_new_col};
    local_a_matrix.start_.resize(ext_num_new_col + 1);
    local_a_matrix.start_[ext_num_new_col] = ext_num_new_nz;
    local_a_matrix.index_ = {ext_a_index, ext_a_index + ext_num_new_nz};
    local_a_matrix.value_ = {ext_a_value, ext_a_value + ext_num_new_nz};
    // Assess the matrix rows
    return_status =
        interpretCallStatus(options_.log_options,
                            local_a_matrix.assess(options.log_options, "LP",
                                                  options.small_matrix_value,
                                                  options.large_matrix_value),
                            return_status, "assessMatrix");
    if (return_status == HighsStatus::kError) return return_status;
  } else {
    // No nonzeros so, whether the constraint matrix is column-wise or
    // row-wise, adding the empty matrix is trivial. Complete the
    // setup of an empty column-wise HighsSparseMatrix of the new
    // matrix columns
    local_a_matrix.start_.assign(ext_num_new_col + 1, 0);
  }
  // Append the columns to LP matrix
  lp.a_matrix_.addCols(local_a_matrix);
  if (lp_has_scaling) {
    // Extend the column scaling factors
    scale.col.resize(newNumCol);
    for (HighsInt iCol = 0; iCol < ext_num_new_col; iCol++)
      scale.col[lp.num_col_ + iCol] = 1.0;
    scale.num_col = newNumCol;
    // Apply the existing row scaling to the new columns
    local_a_matrix.applyRowScale(scale);
    // Consider applying column scaling to the new columns.
    local_a_matrix.considerColScaling(options.allowed_matrix_scale_factor,
                                      &scale.col[lp.num_col_]);
  }
  // Update the basis corresponding to new nonbasic columns
  if (useful_basis) appendNonbasicColsToBasisInterface(ext_num_new_col);

  // Possibly add column names
  lp.addColNames("", ext_num_new_col);

  // Increase the number of columns in the LP
  lp.num_col_ += ext_num_new_col;
  assert(lpDimensionsOk("addCols", lp, options.log_options));

  // Interpret possible introduction of infinite costs
  lp.has_infinite_cost_ = lp.has_infinite_cost_ || local_has_infinite_cost;
  assert(lp.has_infinite_cost_ == lp.hasInfiniteCost(options.infinite_cost));

  // Deduce the consequences of adding new columns
  invalidateModelStatusSolutionAndInfo();

  // Determine any implications for simplex data
  ekk_instance_.addCols(lp, local_a_matrix);

  // Extend any Hessian with zeros on the diagonal
  if (this->model_.hessian_.dim_)
    completeHessian(lp.num_col_, this->model_.hessian_);
  return return_status;
}

HighsStatus Highs::addRowsInterface(HighsInt ext_num_new_row,
                                    const double* ext_row_lower,
                                    const double* ext_row_upper,
                                    HighsInt ext_num_new_nz,
                                    const HighsInt* ext_ar_start,
                                    const HighsInt* ext_ar_index,
                                    const double* ext_ar_value) {
  // addRows is fundamentally different from addCols, since the new
  // matrix data are held row-wise, so we have to insert data into the
  // column-wise matrix of the LP.
  if (kExtendInvertWhenAddingRows) {
    if (ekk_instance_.status_.has_nla)
      ekk_instance_.debugNlaCheckInvert("Start of Highs::addRowsInterface",
                                        kHighsDebugLevelExpensive + 1);
  }
  HighsStatus return_status = HighsStatus::kOk;
  HighsOptions& options = options_;
  if (ext_num_new_row < 0) return HighsStatus::kError;
  if (ext_num_new_nz < 0) return HighsStatus::kError;
  if (ext_num_new_row == 0) return HighsStatus::kOk;
  if (ext_num_new_row > 0)
    if (isRowDataNull(options.log_options, ext_row_lower, ext_row_upper))
      return HighsStatus::kError;
  if (ext_num_new_nz > 0)
    if (isMatrixDataNull(options.log_options, ext_ar_start, ext_ar_index,
                         ext_ar_value))
      return HighsStatus::kError;

  HighsLp& lp = model_.lp_;
  HighsBasis& basis = basis_;
  HighsScale& scale = lp.scale_;
  bool& useful_basis = basis.useful;

  bool& lp_has_scaling = lp.scale_.has_scaling;

  // Check that if nonzeros are to be added then the model has a positive number
  // of columns
  if (lp.num_col_ <= 0 && ext_num_new_nz > 0) return HighsStatus::kError;

  // Record the new number of rows
  HighsInt newNumRow = lp.num_row_ + ext_num_new_row;

  HighsIndexCollection index_collection;
  index_collection.dimension_ = ext_num_new_row;
  index_collection.is_interval_ = true;
  index_collection.from_ = 0;
  index_collection.to_ = ext_num_new_row - 1;
  // Take a copy of the bounds that can be normalised
  std::vector<double> local_rowLower{ext_row_lower,
                                     ext_row_lower + ext_num_new_row};
  std::vector<double> local_rowUpper{ext_row_upper,
                                     ext_row_upper + ext_num_new_row};

  return_status = interpretCallStatus(
      options_.log_options,
      assessBounds(options, "Row", lp.num_row_, index_collection,
                   local_rowLower, local_rowUpper, options.infinite_bound),
      return_status, "assessBounds");
  if (return_status == HighsStatus::kError) return return_status;
  if (lp.user_bound_scale_) {
    // Assess and apply any user bound scaling
    if (!boundScaleOk(local_rowLower, local_rowUpper, lp.user_bound_scale_,
                      options_.infinite_bound)) {
      highsLogUser(options_.log_options, HighsLogType::kError,
                   "User bound scaling yields infinite bound\n");
      return HighsStatus::kError;
    }
    double bound_scale_value = std::pow(2, lp.user_bound_scale_);
    for (HighsInt iRow = 0; iRow < ext_num_new_row; iRow++) {
      local_rowLower[iRow] *= bound_scale_value;
      local_rowUpper[iRow] *= bound_scale_value;
    }
  }

  // Append the rows to the LP vectors
  appendRowsToLpVectors(lp, ext_num_new_row, local_rowLower, local_rowUpper);

  // Form a row-wise HighsSparseMatrix of the new matrix rows so that
  // is easy to handle and, if there are nonzeros, it can be
  // normalised
  HighsSparseMatrix local_ar_matrix;
  local_ar_matrix.num_col_ = lp.num_col_;
  local_ar_matrix.num_row_ = ext_num_new_row;
  local_ar_matrix.format_ = MatrixFormat::kRowwise;
  if (ext_num_new_nz) {
    local_ar_matrix.start_ = {ext_ar_start, ext_ar_start + ext_num_new_row};
    local_ar_matrix.start_.resize(ext_num_new_row + 1);
    local_ar_matrix.start_[ext_num_new_row] = ext_num_new_nz;
    local_ar_matrix.index_ = {ext_ar_index, ext_ar_index + ext_num_new_nz};
    local_ar_matrix.value_ = {ext_ar_value, ext_ar_value + ext_num_new_nz};
    // Assess the matrix columns
    return_status =
        interpretCallStatus(options_.log_options,
                            local_ar_matrix.assess(options.log_options, "LP",
                                                   options.small_matrix_value,
                                                   options.large_matrix_value),
                            return_status, "assessMatrix");
    if (return_status == HighsStatus::kError) return return_status;
  } else {
    // No nonzeros so, whether the constraint matrix is row-wise or
    // column-wise, adding the empty matrix is trivial. Complete the
    // setup of an empty row-wise HighsSparseMatrix of the new matrix
    // rows
    local_ar_matrix.start_.assign(ext_num_new_row + 1, 0);
  }
  // Append the rows to LP matrix
  lp.a_matrix_.addRows(local_ar_matrix);
  if (lp_has_scaling) {
    // Extend the row scaling factors
    scale.row.resize(newNumRow);
    for (HighsInt iRow = 0; iRow < ext_num_new_row; iRow++)
      scale.row[lp.num_row_ + iRow] = 1.0;
    scale.num_row = newNumRow;
    // Apply the existing column scaling to the new rows
    local_ar_matrix.applyColScale(scale);
    // Consider applying row scaling to the new rows.
    local_ar_matrix.considerRowScaling(options.allowed_matrix_scale_factor,
                                       &scale.row[lp.num_row_]);
  }
  // Update the basis corresponding to new basic rows
  if (useful_basis) appendBasicRowsToBasisInterface(ext_num_new_row);

  // Possibly add row names
  lp.addRowNames("", ext_num_new_row);

  // Increase the number of rows in the LP
  lp.num_row_ += ext_num_new_row;
  assert(lpDimensionsOk("addRows", lp, options.log_options));

  // Deduce the consequences of adding new rows
  invalidateModelStatusSolutionAndInfo();
  // Determine any implications for simplex data
  ekk_instance_.addRows(lp, local_ar_matrix);

  return return_status;
}

static void deleteBasisEntries(std::vector<HighsBasisStatus>& status,
                               bool& deleted_basic, bool& deleted_nonbasic,
                               const HighsIndexCollection& index_collection,
                               const HighsInt entry_dim) {
  assert(ok(index_collection));
  assert(static_cast<size_t>(entry_dim) == status.size());
  HighsInt from_k;
  HighsInt to_k;
  limits(index_collection, from_k, to_k);
  if (from_k > to_k) return;

  HighsInt delete_from_entry;
  HighsInt delete_to_entry;
  HighsInt keep_from_entry;
  HighsInt keep_to_entry = -1;
  HighsInt current_set_entry = 0;
  HighsInt new_num_entry = 0;
  deleted_basic = false;
  deleted_nonbasic = false;
  for (HighsInt k = from_k; k <= to_k; k++) {
    updateOutInIndex(index_collection, delete_from_entry, delete_to_entry,
                     keep_from_entry, keep_to_entry, current_set_entry);
    // Account for the initial entries being kept
    if (k == from_k) new_num_entry = delete_from_entry;
    // Identify whether a basic or a nonbasic entry has been deleted
    for (HighsInt entry = delete_from_entry; entry <= delete_to_entry;
         entry++) {
      if (status[entry] == HighsBasisStatus::kBasic) {
        deleted_basic = true;
      } else {
        deleted_nonbasic = true;
      }
    }
    if (delete_to_entry >= entry_dim - 1) break;
    for (HighsInt entry = keep_from_entry; entry <= keep_to_entry; entry++) {
      status[new_num_entry] = status[entry];
      new_num_entry++;
    }
    if (keep_to_entry >= entry_dim - 1) break;
  }
  status.resize(new_num_entry);
}

static void deleteBasisCols(HighsBasis& basis,
                            const HighsIndexCollection& index_collection,
                            const HighsInt original_num_col) {
  bool deleted_basic;
  bool deleted_nonbasic;
  deleteBasisEntries(basis.col_status, deleted_basic, deleted_nonbasic,
                     index_collection, original_num_col);
  if (deleted_basic) basis.valid = false;
}

static void deleteBasisRows(HighsBasis& basis,
                            const HighsIndexCollection& index_collection,
                            const HighsInt original_num_row) {
  bool deleted_basic;
  bool deleted_nonbasic;
  deleteBasisEntries(basis.row_status, deleted_basic, deleted_nonbasic,
                     index_collection, original_num_row);
  if (deleted_nonbasic) basis.valid = false;
}

void Highs::deleteColsInterface(HighsIndexCollection& index_collection) {
  HighsLp& lp = model_.lp_;
  HighsBasis& basis = basis_;
  lp.ensureColwise();

  // Keep a copy of the original number of columns to check whether
  // any columns have been removed, and if there is mask to be updated
  HighsInt original_num_col = lp.num_col_;

  lp.deleteCols(index_collection);
  model_.hessian_.deleteCols(index_collection);
  // Bail out if no columns were actually deleted
  if (lp.num_col_ == original_num_col) return;

  assert(lp.num_col_ < original_num_col);

  // Nontrivial deletion so reset the model_status and update any
  // Highs basis
  model_status_ = HighsModelStatus::kNotset;
  if (basis_.useful) {
    assert(basis_.col_status.size() == static_cast<size_t>(original_num_col));
    // Have a full set of column basis status values, so maintain
    // them, and only invalidate the basis if a basic column has been
    // deleted
    deleteBasisCols(basis_, index_collection, original_num_col);
  } else {
    assert(!basis.valid);
  }

  if (lp.scale_.has_scaling) {
    deleteScale(lp.scale_.col, index_collection);
    lp.scale_.col.resize(lp.num_col_);
    lp.scale_.num_col = lp.num_col_;
  }
  // Deduce the consequences of deleting columns
  invalidateModelStatusSolutionAndInfo();

  // Determine any implications for simplex data
  ekk_instance_.deleteCols(index_collection);

  if (index_collection.is_mask_) {
    // Set the mask values to indicate the new index value of the
    // remaining columns
    HighsInt new_col = 0;
    for (HighsInt col = 0; col < original_num_col; col++) {
      if (!index_collection.mask_[col]) {
        index_collection.mask_[col] = new_col;
        new_col++;
      } else {
        index_collection.mask_[col] = -1;
      }
    }
    assert(new_col == lp.num_col_);
  }
  assert(lpDimensionsOk("deleteCols", lp, options_.log_options));
  lp.col_hash_.name2index.clear();
}

void Highs::deleteRowsInterface(HighsIndexCollection& index_collection) {
  HighsLp& lp = model_.lp_;
  HighsBasis& basis = basis_;
  lp.ensureColwise();
  // Keep a copy of the original number of rows to check whether
  // any rows have been removed, and if there is mask to be updated
  HighsInt original_num_row = lp.num_row_;

  lp.deleteRows(index_collection);
  // Bail out if no rows were actually deleted
  if (lp.num_row_ == original_num_row) return;

  assert(lp.num_row_ < original_num_row);

  // Nontrivial deletion so reset the model_status and update any
  // Highs basis
  model_status_ = HighsModelStatus::kNotset;
  if (basis_.useful) {
    assert(basis_.row_status.size() == static_cast<size_t>(original_num_row));
    // Have a full set of row basis status values, so maintain them,
    // and only invalidate the basis if a nonbasic row has been
    // deleted
    deleteBasisRows(basis_, index_collection, original_num_row);
  } else {
    assert(!basis.valid);
  }

  if (lp.scale_.has_scaling) {
    deleteScale(lp.scale_.row, index_collection);
    lp.scale_.row.resize(lp.num_row_);
    lp.scale_.num_row = lp.num_row_;
  }
  // Deduce the consequences of deleting rows
  invalidateModelStatusSolutionAndInfo();

  // Determine any implications for simplex data
  ekk_instance_.deleteRows(index_collection);
  if (index_collection.is_mask_) {
    HighsInt new_row = 0;
    for (HighsInt row = 0; row < original_num_row; row++) {
      if (!index_collection.mask_[row]) {
        index_collection.mask_[row] = new_row;
        new_row++;
      } else {
        index_collection.mask_[row] = -1;
      }
    }
    assert(new_row == lp.num_row_);
  }
  assert(lpDimensionsOk("deleteRows", lp, options_.log_options));
  lp.row_hash_.name2index.clear();
}

void Highs::getColsInterface(const HighsIndexCollection& index_collection,
                             HighsInt& num_col, double* cost, double* lower,
                             double* upper, HighsInt& num_nz, HighsInt* start,
                             HighsInt* index, double* value) const {
  const HighsLp& lp = model_.lp_;
  if (lp.a_matrix_.isColwise()) {
    getSubVectors(index_collection, lp.num_col_, lp.col_cost_.data(),
                  lp.col_lower_.data(), lp.col_upper_.data(), lp.a_matrix_,
                  num_col, cost, lower, upper, num_nz, start, index, value);
  } else {
    getSubVectorsTranspose(index_collection, lp.num_col_, lp.col_cost_.data(),
                           lp.col_lower_.data(), lp.col_upper_.data(),
                           lp.a_matrix_, num_col, cost, lower, upper, num_nz,
                           start, index, value);
  }
}

void Highs::getRowsInterface(const HighsIndexCollection& index_collection,
                             HighsInt& num_row, double* lower, double* upper,
                             HighsInt& num_nz, HighsInt* start, HighsInt* index,
                             double* value) const {
  const HighsLp& lp = model_.lp_;
  if (lp.a_matrix_.isColwise()) {
    getSubVectorsTranspose(index_collection, lp.num_row_, nullptr,
                           lp.row_lower_.data(), lp.row_upper_.data(),
                           lp.a_matrix_, num_row, nullptr, lower, upper, num_nz,
                           start, index, value);
  } else {
    getSubVectors(index_collection, lp.num_row_, nullptr, lp.row_lower_.data(),
                  lp.row_upper_.data(), lp.a_matrix_, num_row, nullptr, lower,
                  upper, num_nz, start, index, value);
  }
}

void Highs::getCoefficientInterface(const HighsInt ext_row,
                                    const HighsInt ext_col,
                                    double& value) const {
  const HighsLp& lp = model_.lp_;
  assert(0 <= ext_row && ext_row < lp.num_row_);
  assert(0 <= ext_col && ext_col < lp.num_col_);
  value = 0;

  if (lp.a_matrix_.isColwise()) {
    for (HighsInt el = lp.a_matrix_.start_[ext_col];
         el < lp.a_matrix_.start_[ext_col + 1]; el++) {
      if (lp.a_matrix_.index_[el] == ext_row) {
        value = lp.a_matrix_.value_[el];
        break;
      }
    }
  } else {
    for (HighsInt el = lp.a_matrix_.start_[ext_row];
         el < lp.a_matrix_.start_[ext_row + 1]; el++) {
      if (lp.a_matrix_.index_[el] == ext_col) {
        value = lp.a_matrix_.value_[el];
        break;
      }
    }
  }
}

HighsStatus Highs::changeIntegralityInterface(
    HighsIndexCollection& index_collection, const HighsVarType* integrality) {
  HighsInt num_integrality = dataSize(index_collection);
  // If a non-positive number of integrality (may) need changing nothing needs
  // to be done
  if (num_integrality <= 0) return HighsStatus::kOk;
  if (highsVarTypeUserDataNotNull(options_.log_options, integrality,
                                  "column integrality"))
    return HighsStatus::kError;
  // Take a copy of the integrality that can be normalised
  std::vector<HighsVarType> local_integrality{integrality,
                                              integrality + num_integrality};
  // If changing the integrality for a set of columns, verify that the
  // set entries are in ascending order
  if (index_collection.is_set_)
    assert(increasingSetOk(index_collection.set_, 0,
                           index_collection.dimension_, true));
  changeLpIntegrality(model_.lp_, index_collection, local_integrality);
  // Deduce the consequences of new integrality
  invalidateModelStatus();
  return HighsStatus::kOk;
}

HighsStatus Highs::changeCostsInterface(HighsIndexCollection& index_collection,
                                        const double* cost) {
  HighsInt num_cost = dataSize(index_collection);
  // If a non-positive number of costs (may) need changing nothing needs to be
  // done
  if (num_cost <= 0) return HighsStatus::kOk;
  if (doubleUserDataNotNull(options_.log_options, cost, "column costs"))
    return HighsStatus::kError;
  // Take a copy of the cost that can be normalised
  std::vector<double> local_colCost{cost, cost + num_cost};
  HighsStatus return_status = HighsStatus::kOk;
  bool local_has_infinite_cost = false;
  return_status = interpretCallStatus(
      options_.log_options,
      assessCosts(options_, 0, index_collection, local_colCost,
                  local_has_infinite_cost, options_.infinite_cost),
      return_status, "assessCosts");
  if (return_status == HighsStatus::kError) return return_status;
  HighsLp& lp = model_.lp_;
  if (lp.user_cost_scale_) {
    // Assess and apply any user cost scaling
    if (!costScaleOk(local_colCost, lp.user_cost_scale_,
                     options_.infinite_cost)) {
      highsLogUser(options_.log_options, HighsLogType::kError,
                   "User cost scaling yields infinite cost\n");
      return HighsStatus::kError;
    }
    double cost_scale_value = std::pow(2, lp.user_cost_scale_);
    for (HighsInt iCol = 0; iCol < num_cost; iCol++)
      local_colCost[iCol] *= cost_scale_value;
  }
  changeLpCosts(lp, index_collection, local_colCost, options_.infinite_cost);

  // Interpret possible introduction of infinite costs
  lp.has_infinite_cost_ = lp.has_infinite_cost_ || local_has_infinite_cost;
  assert(lp.has_infinite_cost_ == lp.hasInfiniteCost(options_.infinite_cost));

  // Deduce the consequences of new costs
  invalidateModelStatusSolutionAndInfo();
  // Determine any implications for simplex data
  ekk_instance_.updateStatus(LpAction::kNewCosts);
  return HighsStatus::kOk;
}

HighsStatus Highs::changeColBoundsInterface(
    HighsIndexCollection& index_collection, const double* col_lower,
    const double* col_upper) {
  HighsInt num_col_bounds = dataSize(index_collection);
  // If a non-positive number of costs (may) need changing nothing needs to be
  // done
  if (num_col_bounds <= 0) return HighsStatus::kOk;
  bool null_data = false;
  null_data = doubleUserDataNotNull(options_.log_options, col_lower,
                                    "column lower bounds") ||
              null_data;
  null_data = doubleUserDataNotNull(options_.log_options, col_upper,
                                    "column upper bounds") ||
              null_data;
  if (null_data) return HighsStatus::kError;
  // Take a copy of the cost that can be normalised
  std::vector<double> local_colLower{col_lower, col_lower + num_col_bounds};
  std::vector<double> local_colUpper{col_upper, col_upper + num_col_bounds};
  // If changing the bounds for a set of columns, ensure that the
  // set and data are in ascending order
  if (index_collection.is_set_)
    sortSetData(index_collection.set_num_entries_, index_collection.set_,
                col_lower, col_upper, NULL, local_colLower.data(),
                local_colUpper.data(), NULL);
  HighsStatus return_status = HighsStatus::kOk;
  return_status = interpretCallStatus(
      options_.log_options,
      assessBounds(options_, "col", 0, index_collection, local_colLower,
                   local_colUpper, options_.infinite_bound),
      return_status, "assessBounds");
  if (return_status == HighsStatus::kError) return return_status;
  HighsLp& lp = model_.lp_;
  if (lp.user_bound_scale_) {
    // Assess and apply any user bound scaling
    if (!boundScaleOk(local_colLower, local_colUpper, lp.user_bound_scale_,
                      options_.infinite_bound)) {
      highsLogUser(options_.log_options, HighsLogType::kError,
                   "User bound scaling yields infinite bound\n");
      return HighsStatus::kError;
    }
    double bound_scale_value = std::pow(2, lp.user_bound_scale_);
    for (HighsInt iCol = 0; iCol < num_col_bounds; iCol++) {
      local_colLower[iCol] *= bound_scale_value;
      local_colUpper[iCol] *= bound_scale_value;
    }
  }

  changeLpColBounds(lp, index_collection, local_colLower, local_colUpper);
  // Update HiGHS basis status and (any) simplex move status of
  // nonbasic variables whose bounds have changed
  setNonbasicStatusInterface(index_collection, true);
  // Deduce the consequences of new col bounds
  invalidateModelStatusSolutionAndInfo();
  // Determine any implications for simplex data
  ekk_instance_.updateStatus(LpAction::kNewBounds);
  return HighsStatus::kOk;
}

HighsStatus Highs::changeRowBoundsInterface(
    HighsIndexCollection& index_collection, const double* lower,
    const double* upper) {
  HighsInt num_row_bounds = dataSize(index_collection);
  // If a non-positive number of costs (may) need changing nothing needs to be
  // done
  if (num_row_bounds <= 0) return HighsStatus::kOk;
  bool null_data = false;
  null_data =
      doubleUserDataNotNull(options_.log_options, lower, "row lower bounds") ||
      null_data;
  null_data =
      doubleUserDataNotNull(options_.log_options, upper, "row upper bounds") ||
      null_data;
  if (null_data) return HighsStatus::kError;
  // Take a copy of the cost that can be normalised
  std::vector<double> local_rowLower{lower, lower + num_row_bounds};
  std::vector<double> local_rowUpper{upper, upper + num_row_bounds};
  // If changing the bounds for a set of rows, ensure that the
  // set and data are in ascending order
  if (index_collection.is_set_)
    sortSetData(index_collection.set_num_entries_, index_collection.set_, lower,
                upper, NULL, local_rowLower.data(), local_rowUpper.data(),
                NULL);
  HighsStatus return_status = HighsStatus::kOk;
  return_status = interpretCallStatus(
      options_.log_options,
      assessBounds(options_, "row", 0, index_collection, local_rowLower,
                   local_rowUpper, options_.infinite_bound),
      return_status, "assessBounds");
  if (return_status == HighsStatus::kError) return return_status;
  HighsLp& lp = model_.lp_;
  if (lp.user_bound_scale_) {
    // Assess and apply any user bound scaling
    if (!boundScaleOk(local_rowLower, local_rowUpper, lp.user_bound_scale_,
                      options_.infinite_bound)) {
      highsLogUser(options_.log_options, HighsLogType::kError,
                   "User bound scaling yields infinite bound\n");
      return HighsStatus::kError;
    }
    double bound_scale_value = std::pow(2, lp.user_bound_scale_);
    for (HighsInt iRow = 0; iRow < num_row_bounds; iRow++) {
      local_rowLower[iRow] *= bound_scale_value;
      local_rowUpper[iRow] *= bound_scale_value;
    }
  }

  changeLpRowBounds(lp, index_collection, local_rowLower, local_rowUpper);
  // Update HiGHS basis status and (any) simplex move status of
  // nonbasic variables whose bounds have changed
  setNonbasicStatusInterface(index_collection, false);
  // Deduce the consequences of new row bounds
  invalidateModelStatusSolutionAndInfo();
  // Determine any implications for simplex data
  ekk_instance_.updateStatus(LpAction::kNewBounds);
  return HighsStatus::kOk;
}

// Change a single coefficient in the matrix
void Highs::changeCoefficientInterface(const HighsInt ext_row,
                                       const HighsInt ext_col,
                                       const double ext_new_value) {
  HighsLp& lp = model_.lp_;
  // Ensure that the LP is column-wise
  lp.ensureColwise();
  assert(0 <= ext_row && ext_row < lp.num_row_);
  assert(0 <= ext_col && ext_col < lp.num_col_);
  const bool zero_new_value =
      std::fabs(ext_new_value) <= options_.small_matrix_value;
  changeLpMatrixCoefficient(lp, ext_row, ext_col, ext_new_value,
                            zero_new_value);
  // Deduce the consequences of a changed element
  //
  // ToDo: Can do something more intelligent if element is in nonbasic
  // column
  //
  const bool basic_column =
      this->basis_.col_status[ext_col] == HighsBasisStatus::kBasic;
  //
  // For now, treat it as if it's a new row
  invalidateModelStatusSolutionAndInfo();

  if (basic_column) {
    // Basis is retained, but is has to be viewed as alien, since the
    // basis matrix has changed
    this->basis_.was_alien = true;
    this->basis_.alien = true;
  }

  // Determine any implications for simplex data
  ekk_instance_.updateStatus(LpAction::kNewRows);
}

HighsStatus Highs::scaleColInterface(const HighsInt col,
                                     const double scale_value) {
  HighsStatus return_status = HighsStatus::kOk;
  HighsLp& lp = model_.lp_;
  HighsBasis& basis = basis_;
  HighsSimplexStatus& simplex_status = ekk_instance_.status_;

  // Ensure that the LP is column-wise
  lp.ensureColwise();
  if (col < 0) return HighsStatus::kError;
  if (col >= lp.num_col_) return HighsStatus::kError;
  if (!scale_value) return HighsStatus::kError;

  return_status = interpretCallStatus(options_.log_options,
                                      applyScalingToLpCol(lp, col, scale_value),
                                      return_status, "applyScalingToLpCol");
  if (return_status == HighsStatus::kError) return return_status;

  if (scale_value < 0 && basis.valid) {
    // Negative, so flip any nonbasic status
    if (basis.col_status[col] == HighsBasisStatus::kLower) {
      basis.col_status[col] = HighsBasisStatus::kUpper;
    } else if (basis.col_status[col] == HighsBasisStatus::kUpper) {
      basis.col_status[col] = HighsBasisStatus::kLower;
    }
  }
  if (simplex_status.initialised_for_solve) {
    SimplexBasis& simplex_basis = ekk_instance_.basis_;
    if (scale_value < 0 && simplex_status.has_basis) {
      // Negative, so flip any nonbasic status
      if (simplex_basis.nonbasicMove_[col] == kNonbasicMoveUp) {
        simplex_basis.nonbasicMove_[col] = kNonbasicMoveDn;
      } else if (simplex_basis.nonbasicMove_[col] == kNonbasicMoveDn) {
        simplex_basis.nonbasicMove_[col] = kNonbasicMoveUp;
      }
    }
  }
  // Deduce the consequences of a scaled column
  invalidateModelStatusSolutionAndInfo();

  // Determine any implications for simplex data
  ekk_instance_.updateStatus(LpAction::kScaledCol);
  return HighsStatus::kOk;
}

HighsStatus Highs::scaleRowInterface(const HighsInt row,
                                     const double scale_value) {
  HighsStatus return_status = HighsStatus::kOk;
  HighsLp& lp = model_.lp_;
  HighsBasis& basis = basis_;
  HighsSimplexStatus& simplex_status = ekk_instance_.status_;

  // Ensure that the LP is column-wise
  lp.ensureColwise();

  if (row < 0) return HighsStatus::kError;
  if (row >= lp.num_row_) return HighsStatus::kError;
  if (!scale_value) return HighsStatus::kError;

  return_status = interpretCallStatus(options_.log_options,
                                      applyScalingToLpRow(lp, row, scale_value),
                                      return_status, "applyScalingToLpRow");
  if (return_status == HighsStatus::kError) return return_status;

  if (scale_value < 0 && basis.valid) {
    // Negative, so flip any nonbasic status
    if (basis.row_status[row] == HighsBasisStatus::kLower) {
      basis.row_status[row] = HighsBasisStatus::kUpper;
    } else if (basis.row_status[row] == HighsBasisStatus::kUpper) {
      basis.row_status[row] = HighsBasisStatus::kLower;
    }
  }
  if (simplex_status.initialised_for_solve) {
    SimplexBasis& simplex_basis = ekk_instance_.basis_;
    if (scale_value < 0 && simplex_status.has_basis) {
      // Negative, so flip any nonbasic status
      const HighsInt var = lp.num_col_ + row;
      if (simplex_basis.nonbasicMove_[var] == kNonbasicMoveUp) {
        simplex_basis.nonbasicMove_[var] = kNonbasicMoveDn;
      } else if (simplex_basis.nonbasicMove_[var] == kNonbasicMoveDn) {
        simplex_basis.nonbasicMove_[var] = kNonbasicMoveUp;
      }
    }
  }
  // Deduce the consequences of a scaled row
  invalidateModelStatusSolutionAndInfo();

  // Determine any implications for simplex data
  ekk_instance_.updateStatus(LpAction::kScaledRow);
  return HighsStatus::kOk;
}

void Highs::setNonbasicStatusInterface(
    const HighsIndexCollection& index_collection, const bool columns) {
  HighsBasis& highs_basis = basis_;
  if (!highs_basis.valid) return;
  const bool has_simplex_basis = ekk_instance_.status_.has_basis;
  SimplexBasis& simplex_basis = ekk_instance_.basis_;
  HighsLp& lp = model_.lp_;

  assert(ok(index_collection));
  HighsInt from_k;
  HighsInt to_k;
  limits(index_collection, from_k, to_k);
  HighsInt ix_dim;
  if (columns) {
    ix_dim = lp.num_col_;
  } else {
    ix_dim = lp.num_row_;
  }
  // Surely this is checked elsewhere
  assert(0 <= from_k && to_k < ix_dim);
  assert(from_k <= to_k);
  HighsInt set_from_ix;
  HighsInt set_to_ix;
  HighsInt ignore_from_ix;
  HighsInt ignore_to_ix = -1;
  HighsInt current_set_entry = 0;
  // Given a basic-nonbasic partition, all status settings are defined
  // by the bounds unless boxed, in which case any definitive (ie not
  // just kNonbasic) existing status is retained. Otherwise, set to
  // bound nearer to zero
  for (HighsInt k = from_k; k <= to_k; k++) {
    updateOutInIndex(index_collection, set_from_ix, set_to_ix, ignore_from_ix,
                     ignore_to_ix, current_set_entry);
    assert(set_to_ix < ix_dim);
    assert(ignore_to_ix < ix_dim);
    if (columns) {
      for (HighsInt iCol = set_from_ix; iCol <= set_to_ix; iCol++) {
        if (highs_basis.col_status[iCol] == HighsBasisStatus::kBasic) continue;
        // Nonbasic column
        double lower = lp.col_lower_[iCol];
        double upper = lp.col_upper_[iCol];
        HighsBasisStatus status = highs_basis.col_status[iCol];
        int8_t move = kIllegalMoveValue;
        if (lower == upper) {
          if (status == HighsBasisStatus::kNonbasic)
            status = HighsBasisStatus::kLower;
          move = kNonbasicMoveZe;
        } else if (!highs_isInfinity(-lower)) {
          // Finite lower bound so boxed or lower
          if (!highs_isInfinity(upper)) {
            // Finite upper bound so boxed
            if (status == HighsBasisStatus::kNonbasic) {
              // No definitive status, so set to bound nearer to zero
              if (fabs(lower) < fabs(upper)) {
                status = HighsBasisStatus::kLower;
                move = kNonbasicMoveUp;
              } else {
                status = HighsBasisStatus::kUpper;
                move = kNonbasicMoveDn;
              }
            } else if (status == HighsBasisStatus::kLower) {
              move = kNonbasicMoveUp;
            } else {
              move = kNonbasicMoveDn;
            }
          } else {
            // Lower (since upper bound is infinite)
            status = HighsBasisStatus::kLower;
            move = kNonbasicMoveUp;
          }
        } else if (!highs_isInfinity(upper)) {
          // Upper
          status = HighsBasisStatus::kUpper;
          move = kNonbasicMoveDn;
        } else {
          // FREE
          status = HighsBasisStatus::kZero;
          move = kNonbasicMoveZe;
        }
        highs_basis.col_status[iCol] = status;
        if (has_simplex_basis) {
          assert(move != kIllegalMoveValue);
          simplex_basis.nonbasicFlag_[iCol] = kNonbasicFlagTrue;
          simplex_basis.nonbasicMove_[iCol] = move;
        }
      }
    } else {
      for (HighsInt iRow = set_from_ix; iRow <= set_to_ix; iRow++) {
        if (highs_basis.row_status[iRow] == HighsBasisStatus::kBasic) continue;
        // Nonbasic column
        double lower = lp.row_lower_[iRow];
        double upper = lp.row_upper_[iRow];
        HighsBasisStatus status = highs_basis.row_status[iRow];
        int8_t move = kIllegalMoveValue;
        if (lower == upper) {
          if (status == HighsBasisStatus::kNonbasic)
            status = HighsBasisStatus::kLower;
          move = kNonbasicMoveZe;
        } else if (!highs_isInfinity(-lower)) {
          // Finite lower bound so boxed or lower
          if (!highs_isInfinity(upper)) {
            // Finite upper bound so boxed
            if (status == HighsBasisStatus::kNonbasic) {
              // No definitive status, so set to bound nearer to zero
              if (fabs(lower) < fabs(upper)) {
                status = HighsBasisStatus::kLower;
                move = kNonbasicMoveDn;
              } else {
                status = HighsBasisStatus::kUpper;
                move = kNonbasicMoveUp;
              }
            } else if (status == HighsBasisStatus::kLower) {
              move = kNonbasicMoveDn;
            } else {
              move = kNonbasicMoveUp;
            }
          } else {
            // Lower (since upper bound is infinite)
            status = HighsBasisStatus::kLower;
            move = kNonbasicMoveDn;
          }
        } else if (!highs_isInfinity(upper)) {
          // Upper
          status = HighsBasisStatus::kUpper;
          move = kNonbasicMoveUp;
        } else {
          // FREE
          status = HighsBasisStatus::kZero;
          move = kNonbasicMoveZe;
        }
        highs_basis.row_status[iRow] = status;
        if (has_simplex_basis) {
          assert(move != kIllegalMoveValue);
          simplex_basis.nonbasicFlag_[lp.num_col_ + iRow] = kNonbasicFlagTrue;
          simplex_basis.nonbasicMove_[lp.num_col_ + iRow] = move;
        }
      }
    }
    if (ignore_to_ix >= ix_dim - 1) break;
  }
}

void Highs::appendNonbasicColsToBasisInterface(const HighsInt ext_num_new_col) {
  if (ext_num_new_col == 0) return;
  HighsBasis& highs_basis = basis_;
  if (!highs_basis.useful) return;
  const bool has_simplex_basis = ekk_instance_.status_.has_basis;
  SimplexBasis& simplex_basis = ekk_instance_.basis_;
  HighsLp& lp = model_.lp_;

  assert(highs_basis.col_status.size() == static_cast<size_t>(lp.num_col_));
  assert(highs_basis.row_status.size() == static_cast<size_t>(lp.num_row_));

  // Add nonbasic structurals
  HighsInt newNumCol = lp.num_col_ + ext_num_new_col;
  HighsInt newNumTot = newNumCol + lp.num_row_;
  highs_basis.col_status.resize(newNumCol);
  if (has_simplex_basis) {
    simplex_basis.nonbasicFlag_.resize(newNumTot);
    simplex_basis.nonbasicMove_.resize(newNumTot);
    // Shift the row data in basicIndex, nonbasicFlag and nonbasicMove if
    // necessary
    for (HighsInt iRow = lp.num_row_ - 1; iRow >= 0; iRow--) {
      HighsInt iCol = simplex_basis.basicIndex_[iRow];
      if (iCol >= lp.num_col_) {
        // This basic variable is a row, so shift its index
        simplex_basis.basicIndex_[iRow] += ext_num_new_col;
      }
      simplex_basis.nonbasicFlag_[newNumCol + iRow] =
          simplex_basis.nonbasicFlag_[lp.num_col_ + iRow];
      simplex_basis.nonbasicMove_[newNumCol + iRow] =
          simplex_basis.nonbasicMove_[lp.num_col_ + iRow];
    }
  }
  // Make any new columns nonbasic
  for (HighsInt iCol = lp.num_col_; iCol < newNumCol; iCol++) {
    double lower = lp.col_lower_[iCol];
    double upper = lp.col_upper_[iCol];
    HighsBasisStatus status = HighsBasisStatus::kNonbasic;
    int8_t move = kIllegalMoveValue;
    if (lower == upper) {
      // Fixed
      status = HighsBasisStatus::kLower;
      move = kNonbasicMoveZe;
    } else if (!highs_isInfinity(-lower)) {
      // Finite lower bound so boxed or lower
      if (!highs_isInfinity(upper)) {
        // Finite upper bound so boxed
        if (fabs(lower) < fabs(upper)) {
          status = HighsBasisStatus::kLower;
          move = kNonbasicMoveUp;
        } else {
          status = HighsBasisStatus::kUpper;
          move = kNonbasicMoveDn;
        }
      } else {
        // Lower (since upper bound is infinite)
        status = HighsBasisStatus::kLower;
        move = kNonbasicMoveUp;
      }
    } else if (!highs_isInfinity(upper)) {
      // Upper
      status = HighsBasisStatus::kUpper;
      move = kNonbasicMoveDn;
    } else {
      // FREE
      status = HighsBasisStatus::kZero;
      move = kNonbasicMoveZe;
    }
    assert(status != HighsBasisStatus::kNonbasic);
    highs_basis.col_status[iCol] = status;
    if (has_simplex_basis) {
      assert(move != kIllegalMoveValue);
      simplex_basis.nonbasicFlag_[iCol] = kNonbasicFlagTrue;
      simplex_basis.nonbasicMove_[iCol] = move;
    }
  }
}

void Highs::appendBasicRowsToBasisInterface(const HighsInt ext_num_new_row) {
  if (ext_num_new_row == 0) return;
  HighsBasis& highs_basis = basis_;
  if (!highs_basis.useful) return;
  const bool has_simplex_basis = ekk_instance_.status_.has_basis;
  SimplexBasis& simplex_basis = ekk_instance_.basis_;
  HighsLp& lp = model_.lp_;

  assert(highs_basis.col_status.size() == static_cast<size_t>(lp.num_col_));
  assert(highs_basis.row_status.size() == static_cast<size_t>(lp.num_row_));

  // Add basic logicals
  // Add the new rows to the Highs basis
  HighsInt newNumRow = lp.num_row_ + ext_num_new_row;
  highs_basis.row_status.resize(newNumRow);
  for (HighsInt iRow = lp.num_row_; iRow < newNumRow; iRow++)
    highs_basis.row_status[iRow] = HighsBasisStatus::kBasic;
  if (has_simplex_basis) {
    // Add the new rows to the simplex basis
    HighsInt newNumTot = lp.num_col_ + newNumRow;
    simplex_basis.nonbasicFlag_.resize(newNumTot);
    simplex_basis.nonbasicMove_.resize(newNumTot);
    simplex_basis.basicIndex_.resize(newNumRow);
    for (HighsInt iRow = lp.num_row_; iRow < newNumRow; iRow++) {
      simplex_basis.nonbasicFlag_[lp.num_col_ + iRow] = kNonbasicFlagFalse;
      simplex_basis.nonbasicMove_[lp.num_col_ + iRow] = 0;
      simplex_basis.basicIndex_[iRow] = lp.num_col_ + iRow;
    }
  }
}

// Get the basic variables, performing INVERT if necessary
HighsStatus Highs::getBasicVariablesInterface(HighsInt* basic_variables) {
  HighsStatus return_status = HighsStatus::kOk;
  HighsLp& lp = model_.lp_;
  HighsInt num_row = lp.num_row_;
  HighsInt num_col = lp.num_col_;
  HighsSimplexStatus& ekk_status = ekk_instance_.status_;
  // For an LP with no rows the solution is vacuous
  if (num_row == 0) return return_status;
  if (!basis_.valid) {
    highsLogUser(options_.log_options, HighsLogType::kError,
                 "getBasicVariables called without a HiGHS basis\n");
    return HighsStatus::kError;
  }
  if (!ekk_status.has_invert) {
    // The LP has no invert to use, so have to set one up, but only
    // for the current basis, so return_value is the rank deficiency.
    HighsLpSolverObject solver_object(lp, basis_, solution_, info_,
                                      ekk_instance_, callback_, options_,
                                      timer_);
    const bool only_from_known_basis = true;
    return_status = interpretCallStatus(
        options_.log_options,
        formSimplexLpBasisAndFactor(solver_object, only_from_known_basis),
        return_status, "formSimplexLpBasisAndFactor");
    if (return_status != HighsStatus::kOk) return return_status;
  }
  assert(ekk_status.has_invert);

  for (HighsInt row = 0; row < num_row; row++) {
    HighsInt var = ekk_instance_.basis_.basicIndex_[row];
    if (var < num_col) {
      basic_variables[row] = var;
    } else {
      basic_variables[row] = -(1 + var - num_col);
    }
  }
  return return_status;
}

// Solve (transposed) system involving the basis matrix

HighsStatus Highs::basisSolveInterface(const vector<double>& rhs,
                                       double* solution_vector,
                                       HighsInt* solution_num_nz,
                                       HighsInt* solution_indices,
                                       bool transpose) {
  HighsStatus return_status = HighsStatus::kOk;
  HighsLp& lp = model_.lp_;
  HighsInt num_row = lp.num_row_;
  // For an LP with no rows the solution is vacuous
  if (num_row == 0) return return_status;
  // EKK must have an INVERT, but simplex NLA may need the pointer to
  // its LP to be refreshed so that it can use its scale factors
  assert(ekk_instance_.status_.has_invert);
  // Reset the simplex NLA LP and scale pointers for the unscaled LP
  ekk_instance_.setNlaPointersForLpAndScale(lp);
  assert(!lp.is_moved_);
  // Set up solve vector with suitably scaled RHS
  HVector solve_vector;
  solve_vector.setup(num_row);
  solve_vector.clear();
  HighsInt rhs_num_nz = 0;
  for (HighsInt iRow = 0; iRow < num_row; iRow++) {
    if (rhs[iRow]) {
      solve_vector.index[rhs_num_nz++] = iRow;
      solve_vector.array[iRow] = rhs[iRow];
    }
  }
  solve_vector.count = rhs_num_nz;
  //
  // Note that solve_vector.count is just used to determine whether
  // hyper-sparse solves should be used. The indices of the nonzeros
  // in the solution are always accumulated. There's no switch (such
  // as setting solve_vector.count = num_row+1) to not do this.
  //
  // Get expected_density from analysis during simplex solve.
  const double expected_density = 1;
  if (transpose) {
    ekk_instance_.btran(solve_vector, expected_density);
  } else {
    ekk_instance_.ftran(solve_vector, expected_density);
  }
  // Extract the solution
  if (solution_indices == NULL) {
    // Nonzeros in the solution not required
    if (solve_vector.count > num_row) {
      // Solution nonzeros not known
      for (HighsInt iRow = 0; iRow < num_row; iRow++) {
        solution_vector[iRow] = solve_vector.array[iRow];
      }
    } else {
      // Solution nonzeros are known
      for (HighsInt iRow = 0; iRow < num_row; iRow++) solution_vector[iRow] = 0;
      for (HighsInt iX = 0; iX < solve_vector.count; iX++) {
        HighsInt iRow = solve_vector.index[iX];
        solution_vector[iRow] = solve_vector.array[iRow];
      }
    }
  } else {
    // Nonzeros in the solution are required
    if (solve_vector.count > num_row) {
      // Solution nonzeros not known
      solution_num_nz = 0;
      for (HighsInt iRow = 0; iRow < num_row; iRow++) {
        solution_vector[iRow] = 0;
        if (solve_vector.array[iRow]) {
          solution_vector[iRow] = solve_vector.array[iRow];
          solution_indices[*solution_num_nz++] = iRow;
        }
      }
    } else {
      // Solution nonzeros are known
      for (HighsInt iRow = 0; iRow < num_row; iRow++) solution_vector[iRow] = 0;
      for (HighsInt iX = 0; iX < solve_vector.count; iX++) {
        HighsInt iRow = solve_vector.index[iX];
        solution_vector[iRow] = solve_vector.array[iRow];
        solution_indices[iX] = iRow;
      }
      *solution_num_nz = solve_vector.count;
    }
  }
  return HighsStatus::kOk;
}

void Highs::zeroIterationCounts() {
  info_.simplex_iteration_count = 0;
  info_.ipm_iteration_count = 0;
  info_.crossover_iteration_count = 0;
  info_.pdlp_iteration_count = 0;
  info_.qp_iteration_count = 0;
}

HighsStatus Highs::getDualRayInterface(bool& has_dual_ray,
                                       double* dual_ray_value) {
  HighsStatus return_status = HighsStatus::kOk;
  HighsLp& lp = model_.lp_;
  HighsInt num_row = lp.num_row_;
  // For an LP with no rows the dual ray is vacuous
  if (num_row == 0) return return_status;
  bool has_invert = ekk_instance_.status_.has_invert;
  assert(!lp.is_moved_);
  has_dual_ray = ekk_instance_.dual_ray_record_.index != kNoRayIndex;

  // Declare identifiers to save column costs, integrality, any
  // Hessian and the presolve setting, and a flag to know when they
  // should be recovered
  std::vector<double> col_cost;
  HighsHessian hessian;
  bool solve_relaxation;
  std::string presolve;
  bool solve_feasibility_problem = false;
  const bool is_qp = model_.isQp();

  if (dual_ray_value != NULL) {
    // User wants a dual ray whatever
    if (!has_dual_ray || !has_invert) {
      // No dual ray is known, or no INVERT to compute it
      //
      // No point in trying to get a dual ray if the model status is
      // optimal
      if (this->model_status_ == HighsModelStatus::kOptimal) {
        highsLogUser(options_.log_options, HighsLogType::kInfo,
                     "Model status is optimal, so no dual ray is available\n");
        return return_status;
      }
      highsLogUser(options_.log_options, HighsLogType::kInfo,
                   "Solving LP to try to compute dual ray\n");
      // Save the column costs, integrality, any Hessian and the
      // presolve setting
      col_cost = lp.col_cost_;
      if (is_qp) hessian = model_.hessian_;
      this->getOptionValue("presolve", presolve);
      this->getOptionValue("solve_relaxation", solve_relaxation);
      solve_feasibility_problem = true;
      // Zero the costs, integrality and Hessian
      std::vector<double> zero_costs;
      zero_costs.assign(lp.num_col_, 0);
      // Take a copy of the primal ray record, since this will be
      // cleared by calling changeColsCost
      HighsRayRecord primal_ray_record =
          this->ekk_instance_.primal_ray_record_.getRayRecord();
      HighsStatus status =
          this->changeColsCost(0, lp.num_col_ - 1, zero_costs.data());
      assert(status == HighsStatus::kOk);
      // Reinstate the primal ray record
      this->ekk_instance_.primal_ray_record_.setRayRecord(primal_ray_record);
      if (is_qp) {
        HighsHessian zero_hessian;
        this->passHessian(zero_hessian);
      }
      this->setOptionValue("presolve", kHighsOffString);
      this->setOptionValue("solve_relaxation", true);
      HighsStatus call_status = this->run();
      if (call_status != HighsStatus::kOk) return_status = call_status;
      has_dual_ray = ekk_instance_.dual_ray_record_.index != kNoRayIndex;
      has_invert = ekk_instance_.status_.has_invert;
      assert(has_invert);
    }
    if (has_dual_ray) {
      if (ekk_instance_.dual_ray_record_.value.size()) {
        // Dual ray is already computed
        highsLogUser(options_.log_options, HighsLogType::kInfo,
                     "Copying known dual ray\n");
        for (HighsInt iRow = 0; iRow < num_row; iRow++)
          dual_ray_value[iRow] = ekk_instance_.dual_ray_record_.value[iRow];
      } else if (has_invert) {
        // Dual ray is known and can be calculated
        highsLogUser(options_.log_options, HighsLogType::kInfo,
                     "Solving linear system to compute dual ray\n");
        vector<double> rhs;
        HighsInt iRow = ekk_instance_.dual_ray_record_.index;
        rhs.assign(num_row, 0);
        rhs[iRow] = ekk_instance_.dual_ray_record_.sign;
        HighsInt* dual_ray_num_nz = 0;
        basisSolveInterface(rhs, dual_ray_value, dual_ray_num_nz, NULL, true);
        // Now save the dual ray itself
        ekk_instance_.dual_ray_record_.value.resize(num_row);
        for (HighsInt iRow = 0; iRow < num_row; iRow++)
          ekk_instance_.dual_ray_record_.value[iRow] = dual_ray_value[iRow];
      } else {
        assert(!has_invert);
        // Dual ray is known but cannot be calculated
        highsLogUser(options_.log_options, HighsLogType::kError,
                     "No LP invertible representation to compute dual ray\n");
        return_status = HighsStatus::kError;
      }
    } else {
      highsLogUser(options_.log_options, HighsLogType::kInfo,
                   "No dual ray found\n");
      return_status = HighsStatus::kOk;
    }
  }
  if (solve_feasibility_problem) {
    // Feasibility problem has been solved, so any objective-related
    // information has been lost. Reverting the objective function via
    // Highs calls clears info_, so better to just copy the data
    // directly and set the info_ entries that are no longer valid
    lp.col_cost_ = col_cost;
    if (is_qp) model_.hessian_ = hessian;
    this->setOptionValue("presolve", presolve);
    this->setOptionValue("solve_relaxation", solve_relaxation);
    // The relaxation for an infeasible MIP may be feasible - so no
    // ray is generated - so make sure (#2415) that the primal
    // solution status is reset
    this->info_.primal_solution_status = SolutionStatus::kSolutionStatusNone;
    // Modify the objective-related information
    this->info_.dual_solution_status = SolutionStatus::kSolutionStatusNone;
    this->info_.objective_function_value = 0;
    this->info_.invalidateDualKkt();
    if (has_dual_ray) {
      assert(this->info_.num_primal_infeasibilities > 0);
      assert(this->model_status_ == HighsModelStatus::kInfeasible);
    } else {
      // If someone has tried to get a dual ray for a feasible problem
      // - or if the relaxation is feasible - then any model and
      // primal KKT status of the original model has been lost
      this->info_.invalidatePrimalKkt();
      this->model_status_ = HighsModelStatus::kNotset;
    }
  }
  return return_status;
}

HighsStatus Highs::getPrimalRayInterface(bool& has_primal_ray,
                                         double* primal_ray_value) {
  HighsStatus return_status = HighsStatus::kOk;
  HighsLp& lp = model_.lp_;
  HighsInt num_row = lp.num_row_;
  HighsInt num_col = lp.num_col_;
  // For an LP with no rows the primal ray is vacuous
  if (num_row == 0) return return_status;
  if (model_.isQp()) {
    highsLogUser(options_.log_options, HighsLogType::kInfo,
                 "Cannot find primal ray for unbounded QP\n");
    return HighsStatus::kError;
  }
  bool has_invert = ekk_instance_.status_.has_invert;
  assert(!lp.is_moved_);
  has_primal_ray = ekk_instance_.primal_ray_record_.index != kNoRayIndex;

  std::string presolve;
  bool solve_relaxation;
  bool allow_unbounded_or_infeasible;
  bool solve_unboundedness_problem = false;

  if (primal_ray_value != NULL) {
    // User wants a primal ray whatever
    if (!has_primal_ray || !has_invert) {
      // No primal ray is known, or no INVERT to compute it
      //
      // No point in trying to get a primal ray if the model status is
      // optimal
      if (this->model_status_ == HighsModelStatus::kOptimal) {
        highsLogUser(
            options_.log_options, HighsLogType::kInfo,
            "Model status is optimal, so no primal ray is available\n");
        return return_status;
      }
      highsLogUser(options_.log_options, HighsLogType::kInfo,
                   "Solving LP to try to compute primal ray\n");
      this->getOptionValue("presolve", presolve);
      this->getOptionValue("solve_relaxation", solve_relaxation);
      this->getOptionValue("allow_unbounded_or_infeasible",
                           allow_unbounded_or_infeasible);
      solve_unboundedness_problem = true;
      this->setOptionValue("presolve", kHighsOffString);
      this->setOptionValue("solve_relaxation", true);
      this->setOptionValue("allow_unbounded_or_infeasible", false);
      HighsStatus call_status = this->run();
      if (call_status != HighsStatus::kOk) return_status = call_status;
      has_primal_ray = ekk_instance_.primal_ray_record_.index != kNoRayIndex;
      has_invert = ekk_instance_.status_.has_invert;
      assert(has_invert);
    }
    if (has_primal_ray) {
      if (ekk_instance_.primal_ray_record_.value.size()) {
        // Primal ray is already computed
        highsLogUser(options_.log_options, HighsLogType::kInfo,
                     "Copying known primal ray\n");
        for (HighsInt iCol = 0; iCol < num_col; iCol++)
          primal_ray_value[iCol] = ekk_instance_.primal_ray_record_.value[iCol];
        return return_status;
      } else if (has_invert) {
        // Primal ray is known and can be calculated
        highsLogUser(options_.log_options, HighsLogType::kInfo,
                     "Solving linear system to compute primal ray\n");
        HighsInt col = ekk_instance_.primal_ray_record_.index;
        assert(ekk_instance_.basis_.nonbasicFlag_[col] == kNonbasicFlagTrue);
        // Get this pivotal column
        vector<double> rhs;
        vector<double> column;
        column.assign(num_row, 0);
        rhs.assign(num_row, 0);
        lp.ensureColwise();
        HighsInt primal_ray_sign = ekk_instance_.primal_ray_record_.sign;
        if (col < num_col) {
          for (HighsInt iEl = lp.a_matrix_.start_[col];
               iEl < lp.a_matrix_.start_[col + 1]; iEl++)
            rhs[lp.a_matrix_.index_[iEl]] =
                primal_ray_sign * lp.a_matrix_.value_[iEl];
        } else {
          rhs[col - num_col] = primal_ray_sign;
        }
        HighsInt* column_num_nz = 0;
        basisSolveInterface(rhs, column.data(), column_num_nz, NULL, false);
        // Now zero primal_ray_value and scatter the column according to
        // the basic variables.
        for (HighsInt iCol = 0; iCol < num_col; iCol++)
          primal_ray_value[iCol] = 0;
        for (HighsInt iRow = 0; iRow < num_row; iRow++) {
          HighsInt iCol = ekk_instance_.basis_.basicIndex_[iRow];
          if (iCol < num_col) primal_ray_value[iCol] = column[iRow];
        }
        if (col < num_col) primal_ray_value[col] = -primal_ray_sign;
        // Now save the primal ray itself
        ekk_instance_.primal_ray_record_.value.resize(num_col);
        for (HighsInt iCol = 0; iCol < num_col; iCol++)
          ekk_instance_.primal_ray_record_.value[iCol] = primal_ray_value[iCol];
      }
    } else {
      highsLogUser(options_.log_options, HighsLogType::kInfo,
                   "No primal ray found\n");
      return_status = HighsStatus::kOk;
    }
  }
  const bool is_mip = this->model_.isMip();
  if (solve_unboundedness_problem) {
    if (is_mip) {
      // Unboundedness LP has been solved, but that will give dual
      // solution status kInfeasible which, for a MIP is not correct
      this->info_.dual_solution_status = SolutionStatus::kSolutionStatusNone;
      this->info_.invalidateDualKkt();
    }
    // Restore the option values
    this->setOptionValue("presolve", presolve);
    this->setOptionValue("solve_relaxation", solve_relaxation);
    this->setOptionValue("allow_unbounded_or_infeasible",
                         allow_unbounded_or_infeasible);
    if (has_primal_ray) {
      assert(is_mip || this->info_.num_dual_infeasibilities > 0);
      assert(this->model_status_ == HighsModelStatus::kUnbounded);
    }
  }
  return return_status;
}

HighsStatus Highs::getRangingInterface() {
  HighsLpSolverObject solver_object(model_.lp_, basis_, solution_, info_,
                                    ekk_instance_, callback_, options_, timer_);
  solver_object.model_status_ = model_status_;
  return getRangingData(this->ranging_, solver_object);
}

HighsStatus Highs::getIisInterface() {
  if (this->iis_.valid_) return HighsStatus::kOk;
  this->iis_.invalidate();
  HighsLp& lp = model_.lp_;
  // Check for trivial IIS: empty infeasible row or inconsistent bounds
  if (this->iis_.trivial(lp, options_)) return HighsStatus::kOk;
  HighsInt num_row = lp.num_row_;
  if (num_row == 0) {
    // For an LP with no rows, the only scope for infeasibility is
    // inconsistent columns bounds - which has already been assessed,
    // so validate the empty HighsIis instance
    this->iis_.valid_ = true;
    return HighsStatus::kOk;
  }
  const bool ray_option = false;
  //      options_.iis_strategy == kIisStrategyFromRayRowPriority ||
  //      options_.iis_strategy == kIisStrategyFromRayColPriority;
  if (this->model_status_ == HighsModelStatus::kInfeasible && ray_option &&
      !ekk_instance_.status_.has_invert) {
    // Model is known to be infeasible, and a dual ray option is
    // chosen, but it has no INVERT, presumably because infeasibility
    // detected in presolve, so solve without presolve
    std::string presolve = options_.presolve;
    options_.presolve = kHighsOffString;

    HighsIisInfo iis_info;
    iis_info.simplex_time = -this->getRunTime();
    iis_info.simplex_iterations = -info_.simplex_iteration_count;
    HighsStatus run_status = this->run();
    options_.presolve = presolve;
    if (run_status != HighsStatus::kOk) return run_status;
    iis_info.simplex_time += this->getRunTime();
    iis_info.simplex_iterations += -info_.simplex_iteration_count;
    this->iis_.info_.push_back(iis_info);

    // Model should remain infeasible!
    if (this->model_status_ != HighsModelStatus::kInfeasible) {
      highsLogUser(
          options_.log_options, HighsLogType::kError,
          "Model status has switched from %s to %s when solving without "
          "presolve\n",
          this->modelStatusToString(HighsModelStatus::kInfeasible).c_str(),
          this->modelStatusToString(this->model_status_).c_str());
      return HighsStatus::kError;
    }
  }
  const bool has_dual_ray = ekk_instance_.dual_ray_record_.index != kNoRayIndex;
  if (ray_option && !has_dual_ray)
    highsLogUser(
        options_.log_options, HighsLogType::kWarning,
        "No known dual ray from which to compute IIS: using whole model\n");
  std::vector<HighsInt> infeasible_row_subset;
  if (ray_option && has_dual_ray) {
    // Compute the dual ray to identify an infeasible subset of rows
    assert(ekk_instance_.status_.has_invert);
    assert(!lp.is_moved_);
    std::vector<double> rhs;
    HighsInt iRow = ekk_instance_.dual_ray_record_.index;
    rhs.assign(num_row, 0);
    rhs[iRow] = 1;
    std::vector<double> dual_ray_value(num_row);
    HighsInt* dual_ray_num_nz = 0;
    basisSolveInterface(rhs, dual_ray_value.data(), dual_ray_num_nz, NULL,
                        true);
    for (HighsInt iRow = 0; iRow < lp.num_row_; iRow++)
      if (dual_ray_value[iRow]) infeasible_row_subset.push_back(iRow);
  } else {
    // Full LP option chosen or no dual ray to use
    //
    // Working on the whole model so clear all solver data
    this->invalidateSolverData();
    // 1789 Remove this check!
    HighsLp check_lp_before = this->model_.lp_;
    // Apply the elasticity filter to the whole model in order to
    // determine an infeasible subset of rows
    HighsStatus return_status =
        this->elasticityFilter(-1.0, -1.0, 1.0, nullptr, nullptr, nullptr, true,
                               infeasible_row_subset);
    HighsLp check_lp_after = this->model_.lp_;
    assert(check_lp_before.equalButForScalingAndNames(check_lp_after));
    if (return_status != HighsStatus::kOk) return return_status;
  }
  HighsStatus return_status = HighsStatus::kOk;
  if (infeasible_row_subset.size() == 0) {
    // No subset of infeasible rows, so model is feasible
    this->iis_.valid_ = true;
  } else {
    return_status =
        this->iis_.getData(lp, options_, basis_, infeasible_row_subset);
    if (return_status == HighsStatus::kOk) {
      // Existence of non-empty IIS => infeasibility
      if (this->iis_.col_index_.size() > 0 || this->iis_.row_index_.size() > 0)
        this->model_status_ = HighsModelStatus::kInfeasible;
    }
    // Analyse the LP solution data
    const HighsInt num_lp_solved = this->iis_.info_.size();
    double min_time = kHighsInf;
    double sum_time = 0;
    double max_time = 0;
    HighsInt min_iterations = kHighsIInf;
    HighsInt sum_iterations = 0;
    HighsInt max_iterations = 0;
    for (HighsInt iX = 0; iX < num_lp_solved; iX++) {
      double time = this->iis_.info_[iX].simplex_time;
      HighsInt iterations = this->iis_.info_[iX].simplex_iterations;
      min_time = std::min(time, min_time);
      sum_time += time;
      max_time = std::max(time, max_time);
      min_iterations = std::min(iterations, min_iterations);
      sum_iterations += iterations;
      max_iterations = std::max(iterations, max_iterations);
    }
    highsLogUser(options_.log_options, HighsLogType::kInfo,
                 " %d cols, %d rows, %d LPs solved"
                 " (min / average / max) iteration count (%6d / %6.2g / % 6d)"
                 " and time (%6.2f / %6.2f / % 6.2f) \n",
                 int(this->iis_.col_index_.size()),
                 int(this->iis_.row_index_.size()), int(num_lp_solved),
                 int(min_iterations),
                 num_lp_solved > 0 ? (1.0 * sum_iterations) / num_lp_solved : 0,
                 int(max_iterations), min_time,
                 num_lp_solved > 0 ? sum_time / num_lp_solved : 0, max_time);
  }
  return return_status;
}

HighsStatus Highs::elasticityFilterReturn(
    const HighsStatus return_status, const bool feasible_model,
    const HighsInt original_num_col, const HighsInt original_num_row,
    const std::vector<double>& original_col_cost,
    const std::vector<double>& original_col_lower,
    const std::vector<double> original_col_upper,
    const std::vector<HighsVarType> original_integrality) {
  const HighsLp& lp = this->model_.lp_;
  double objective_function_value = info_.objective_function_value;
  // Delete any additional rows and columns, and restore the original
  // column costs and bounds
  HighsStatus run_status;
  run_status = this->deleteRows(original_num_row, lp.num_row_ - 1);
  assert(run_status == HighsStatus::kOk);

  run_status = this->deleteCols(original_num_col, lp.num_col_ - 1);
  assert(run_status == HighsStatus::kOk);
  //
  // Now that deleteRows and deleteCols may yield a valid basis, the
  // lack of dual values triggers an assert in
  // getKktFailures. Ultimately (#2081) the dual values will be
  // available but, for now, make the basis invalid.
  basis_.valid = false;

  run_status =
      this->changeColsCost(0, original_num_col - 1, original_col_cost.data());
  assert(run_status == HighsStatus::kOk);

  run_status =
      this->changeColsBounds(0, original_num_col - 1, original_col_lower.data(),
                             original_col_upper.data());
  assert(run_status == HighsStatus::kOk);

  if (original_integrality.size()) {
    this->changeColsIntegrality(0, original_num_col - 1,
                                original_integrality.data());
    assert(run_status == HighsStatus::kOk);
  }

  assert(lp.num_col_ == original_num_col);
  assert(lp.num_row_ == original_num_row);

  if (return_status == HighsStatus::kOk) {
    // Solution is invalidated by deleting rows and columns, but
    // primal values are correct. Have to recompute row activities,
    // though
    this->model_.lp_.a_matrix_.productQuad(this->solution_.row_value,
                                           this->solution_.col_value);
    this->solution_.value_valid = true;
    // Set the feasibility objective and any KKT failures
    info_.objective_function_value = objective_function_value;
    getKktFailures(options_, model_, solution_, basis_, info_);
    info_.valid = true;
  }

  // If the model is feasible, then the status of model is not known
  if (feasible_model) this->model_status_ = HighsModelStatus::kNotset;

  return return_status;
}

HighsStatus Highs::elasticityFilter(
    const double global_lower_penalty, const double global_upper_penalty,
    const double global_rhs_penalty, const double* local_lower_penalty,
    const double* local_upper_penalty, const double* local_rhs_penalty,
    const bool get_infeasible_row,
    std::vector<HighsInt>& infeasible_row_subset) {
  //  this->writeModel("infeasible.mps");
  // Solve the feasibility relaxation problem for the given penalties,
  // continuing to act as the elasticity filter get_infeasible_row is
  // true, resulting in an infeasibility subset for further refinement
  // as an IIS
  //
  // Construct the e-LP:
  //
  // Constraints L <= Ax <= U; l <= x <= u
  //
  // Transformed to
  //
  // L <= Ax + e_L - e_U <= U,
  //
  // l <=  x + e_l - e_u <= u,
  //
  // where the elastic variables are not used if the corresponding
  // bound is infinite or the local/global penalty is negative.
  //
  // x is free, and the objective is the linear function of the
  // elastic variables given by the local/global penalties
  //
  // col_of_ecol lists the column indices corresponding to the entries in
  // bound_of_col_of_ecol so that the results can be interpreted
  //
  // row_of_ecol lists the row indices corresponding to the entries in
  // bound_of_row_of_ecol so that the results can be interpreted
  std::vector<HighsInt> col_of_ecol;
  std::vector<HighsInt> row_of_ecol;
  std::vector<double> bound_of_row_of_ecol;
  std::vector<double> bound_of_col_of_ecol;
  std::vector<double> erow_lower;
  std::vector<double> erow_upper;
  std::vector<HighsInt> erow_start;
  std::vector<HighsInt> erow_index;
  std::vector<double> erow_value;
  // Accumulate names for ecols and erows, re-using ecol_name for the
  // names of row ecols after defining the names of col ecols
  std::vector<std::string> ecol_name;
  std::vector<std::string> erow_name;
  std::vector<double> ecol_cost;
  std::vector<double> ecol_lower;
  std::vector<double> ecol_upper;
  const HighsLp& lp = this->model_.lp_;
  HighsInt evar_ix = lp.num_col_;
  HighsStatus run_status;
  const bool write_model = false;
  // Take copies of the original model dimensions and column data
  // vectors, as they will be modified in forming the e-LP
  const HighsInt original_num_col = lp.num_col_;
  const HighsInt original_num_row = lp.num_row_;
  const std::vector<double> original_col_cost = lp.col_cost_;
  const std::vector<double> original_col_lower = lp.col_lower_;
  const std::vector<double> original_col_upper = lp.col_upper_;
  const std::vector<HighsVarType> original_integrality = lp.integrality_;
  // Zero the column costs
  std::vector<double> zero_costs;
  zero_costs.assign(original_num_col, 0);
  run_status = this->changeColsCost(0, lp.num_col_ - 1, zero_costs.data());
  assert(run_status == HighsStatus::kOk);

  if (get_infeasible_row && lp.integrality_.size()) {
    // Set any integrality to continuous
    std::vector<HighsVarType> all_continuous;
    all_continuous.assign(original_num_col, HighsVarType::kContinuous);
    run_status =
        this->changeColsIntegrality(0, lp.num_col_ - 1, all_continuous.data());
    assert(run_status == HighsStatus::kOk);
  }

  // For the columns
  const bool has_local_lower_penalty = local_lower_penalty;
  const bool has_global_elastic_lower = global_lower_penalty >= 0;
  const bool has_elastic_lower =
      has_local_lower_penalty || has_global_elastic_lower;
  const bool has_local_upper_penalty = local_upper_penalty;
  const bool has_global_elastic_upper = global_upper_penalty >= 0;
  const bool has_elastic_upper =
      has_local_upper_penalty || has_global_elastic_upper;
  const bool has_elastic_columns = has_elastic_lower || has_elastic_upper;
  // For the rows
  const bool has_local_rhs_penalty = local_rhs_penalty;
  const bool has_global_elastic_rhs = global_rhs_penalty >= 0;
  const bool has_elastic_rows = has_local_rhs_penalty || has_global_elastic_rhs;
  assert(has_elastic_columns || has_elastic_rows);

  const HighsInt col_ecol_offset = lp.num_col_;
  if (has_elastic_columns) {
    // Accumulate bounds to be used for columns
    std::vector<double> col_lower;
    std::vector<double> col_upper;
    // When defining names, need to know the column number
    const bool has_col_names = lp.col_names_.size() > 0;
    erow_start.push_back(0);
    for (HighsInt iCol = 0; iCol < lp.num_col_; iCol++) {
      const double lower = lp.col_lower_[iCol];
      const double upper = lp.col_upper_[iCol];
      // Original bounds used unless e-variable introduced
      col_lower.push_back(lower);
      col_upper.push_back(upper);
      // Free columns have no erow
      if (lower <= -kHighsInf && upper >= kHighsInf) continue;

      // Get the penalty for violating the lower bounds on this column
      const double lower_penalty = has_local_lower_penalty
                                       ? local_lower_penalty[iCol]
                                       : global_lower_penalty;
      // Negative lower penalty and infinite upper bound implies that the
      // bounds cannot be violated
      if (lower_penalty < 0 && upper >= kHighsInf) continue;

      // Get the penalty for violating the upper bounds on this column
      const double upper_penalty = has_local_upper_penalty
                                       ? local_upper_penalty[iCol]
                                       : global_upper_penalty;
      // Infinite upper bound and negative lower penalty implies that the
      // bounds cannot be violated
      if (lower <= -kHighsInf && upper_penalty < 0) continue;
      erow_lower.push_back(lower);
      erow_upper.push_back(upper);
      if (has_col_names)
        erow_name.push_back("row_" + std::to_string(iCol) + "_" +
                            lp.col_names_[iCol] + "_erow");
      // Define the entry for x[iCol]
      erow_index.push_back(iCol);
      erow_value.push_back(1);
      if (lower > -kHighsInf && lower_penalty >= 0) {
        // New e_l variable
        col_of_ecol.push_back(iCol);
        if (has_col_names)
          ecol_name.push_back("col_" + std::to_string(iCol) + "_" +
                              lp.col_names_[iCol] + "_lower");
        // Save the original lower bound on this column and free its
        // lower bound
        bound_of_col_of_ecol.push_back(lower);
        col_lower[iCol] = -kHighsInf;
        erow_index.push_back(evar_ix);
        erow_value.push_back(1);
        ecol_cost.push_back(lower_penalty);
        evar_ix++;
      }
      if (upper < kHighsInf && upper_penalty >= 0) {
        // New e_u variable
        col_of_ecol.push_back(iCol);
        if (has_col_names)
          ecol_name.push_back("col_" + std::to_string(iCol) + "_" +
                              lp.col_names_[iCol] + "_upper");
        // Save the original upper bound on this column and free its
        // upper bound
        bound_of_col_of_ecol.push_back(upper);
        col_upper[iCol] = kHighsInf;
        erow_index.push_back(evar_ix);
        erow_value.push_back(-1);
        ecol_cost.push_back(upper_penalty);
        evar_ix++;
      }
      erow_start.push_back(erow_index.size());
      HighsInt row_nz =
          erow_start[erow_start.size() - 1] - erow_start[erow_start.size() - 2];
      //      printf("eRow for column %d has %d nonzeros\n", int(iCol),
      //      int(row_nz));
      assert(row_nz == 2 || row_nz == 3);
    }
    HighsInt num_new_col = col_of_ecol.size();
    HighsInt num_new_row = erow_start.size() - 1;
    HighsInt num_new_nz = erow_start[num_new_row];
    if (kIisDevReport)
      printf(
          "Elasticity filter: For columns there are %d variables and %d "
          "constraints\n",
          int(num_new_col), int(num_new_row));
    // Apply the original column bound changes
    assert(col_lower.size() == static_cast<size_t>(lp.num_col_));
    assert(col_upper.size() == static_cast<size_t>(lp.num_col_));
    run_status = this->changeColsBounds(0, lp.num_col_ - 1, col_lower.data(),
                                        col_upper.data());
    assert(run_status == HighsStatus::kOk);
    // Add the new columns
    ecol_lower.assign(num_new_col, 0);
    ecol_upper.assign(num_new_col, kHighsInf);
    run_status = this->addCols(num_new_col, ecol_cost.data(), ecol_lower.data(),
                               ecol_upper.data(), 0, nullptr, nullptr, nullptr);
    assert(run_status == HighsStatus::kOk);
    // Add the new rows
    assert(erow_start.size() == static_cast<size_t>(num_new_row + 1));
    assert(erow_index.size() == static_cast<size_t>(num_new_nz));
    assert(erow_value.size() == static_cast<size_t>(num_new_nz));
    run_status = this->addRows(num_new_row, erow_lower.data(),
                               erow_upper.data(), num_new_nz, erow_start.data(),
                               erow_index.data(), erow_value.data());
    assert(run_status == HighsStatus::kOk);
    if (has_col_names) {
      for (HighsInt iCol = 0; iCol < num_new_col; iCol++)
        this->passColName(col_ecol_offset + iCol, ecol_name[iCol]);
      for (HighsInt iRow = 0; iRow < num_new_row; iRow++)
        this->passRowName(original_num_row + iRow, erow_name[iRow]);
    }
    assert(ecol_cost.size() == static_cast<size_t>(num_new_col));
    assert(ecol_lower.size() == static_cast<size_t>(num_new_col));
    assert(ecol_upper.size() == static_cast<size_t>(num_new_col));
    if (write_model) {
      printf("\nAfter adding %d e-rows\n=============\n", int(num_new_col));
      bool output_flag;
      run_status = this->getOptionValue("output_flag", output_flag);
      this->setOptionValue("output_flag", true);
      this->writeModel("");
      this->setOptionValue("output_flag", output_flag);
    }
  }
  const HighsInt row_ecol_offset = lp.num_col_;
  if (has_elastic_rows) {
    // Add the columns corresponding to the e_L and e_U variables for
    // the constraints
    ecol_name.clear();
    ecol_cost.clear();
    std::vector<HighsInt> ecol_start;
    std::vector<HighsInt> ecol_index;
    std::vector<double> ecol_value;
    ecol_start.push_back(0);
    const bool has_row_names = lp.row_names_.size() > 0;
    for (HighsInt iRow = 0; iRow < original_num_row; iRow++) {
      // Get the penalty for violating the bounds on this row
      const double penalty =
          has_local_rhs_penalty ? local_rhs_penalty[iRow] : global_rhs_penalty;
      // Negative penalty implies that the bounds cannot be violated
      if (penalty < 0) continue;
      const double lower = lp.row_lower_[iRow];
      const double upper = lp.row_upper_[iRow];
      if (lower > -kHighsInf) {
        // Create an e-var for the row lower bound
        row_of_ecol.push_back(iRow);
        if (has_row_names)
          ecol_name.push_back("row_" + std::to_string(iRow) + "_" +
                              lp.row_names_[iRow] + "_lower");
        bound_of_row_of_ecol.push_back(lower);
        // Define the sub-matrix column
        ecol_index.push_back(iRow);
        ecol_value.push_back(1);
        ecol_start.push_back(ecol_index.size());
        ecol_cost.push_back(penalty);
        evar_ix++;
      }
      if (upper < kHighsInf) {
        // Create an e-var for the row upper bound
        row_of_ecol.push_back(iRow);
        if (has_row_names)
          ecol_name.push_back("row_" + std::to_string(iRow) + "_" +
                              lp.row_names_[iRow] + "_upper");
        bound_of_row_of_ecol.push_back(upper);
        // Define the sub-matrix column
        ecol_index.push_back(iRow);
        ecol_value.push_back(-1);
        ecol_start.push_back(ecol_index.size());
        ecol_cost.push_back(penalty);
        evar_ix++;
      }
    }
    HighsInt num_new_col = ecol_start.size() - 1;
    HighsInt num_new_nz = ecol_start[num_new_col];
    ecol_lower.assign(num_new_col, 0);
    ecol_upper.assign(num_new_col, kHighsInf);
    assert(ecol_cost.size() == static_cast<size_t>(num_new_col));
    assert(ecol_lower.size() == static_cast<size_t>(num_new_col));
    assert(ecol_upper.size() == static_cast<size_t>(num_new_col));
    assert(ecol_start.size() == static_cast<size_t>(num_new_col + 1));
    assert(ecol_index.size() == static_cast<size_t>(num_new_nz));
    assert(ecol_value.size() == static_cast<size_t>(num_new_nz));
    run_status = this->addCols(num_new_col, ecol_cost.data(), ecol_lower.data(),
                               ecol_upper.data(), num_new_nz, ecol_start.data(),
                               ecol_index.data(), ecol_value.data());
    assert(run_status == HighsStatus::kOk);
    if (has_row_names) {
      for (HighsInt iCol = 0; iCol < num_new_col; iCol++)
        this->passColName(row_ecol_offset + iCol, ecol_name[iCol]);
    }

    if (write_model) {
      bool output_flag;
      printf("\nAfter adding %d e-cols\n=============\n", int(num_new_col));
      run_status = this->getOptionValue("output_flag", output_flag);
      this->setOptionValue("output_flag", true);
      this->writeModel("");
      this->setOptionValue("output_flag", output_flag);
    }
  }

  if (write_model) this->writeModel("elastic.mps");

  // Lambda for gathering data when solving an LP
  auto solveLp = [&]() -> HighsStatus {
    HighsIisInfo iis_info;
    iis_info.simplex_time = -this->getRunTime();
    iis_info.simplex_iterations = -info_.simplex_iteration_count;
    run_status = this->run();
    assert(run_status == HighsStatus::kOk);
    if (run_status != HighsStatus::kOk) return run_status;
    iis_info.simplex_time += this->getRunTime();
    iis_info.simplex_iterations += info_.simplex_iteration_count;
    this->iis_.info_.push_back(iis_info);
    return run_status;
  };

  run_status = solveLp();

  if (run_status != HighsStatus::kOk)
    return elasticityFilterReturn(run_status, false, original_num_col,
                                  original_num_row, original_col_cost,
                                  original_col_lower, original_col_upper,
                                  original_integrality);
  if (kIisDevReport) this->writeSolution("", kSolutionStylePretty);
  // Model status should be optimal, unless model is unbounded
  assert(this->model_status_ == HighsModelStatus::kOptimal ||
         this->model_status_ == HighsModelStatus::kUnbounded);

  if (!get_infeasible_row)
    return elasticityFilterReturn(HighsStatus::kOk, false, original_num_col,
                                  original_num_row, original_col_cost,
                                  original_col_lower, original_col_upper,
                                  original_integrality);
  const HighsSolution& solution = this->getSolution();
  // Now fix e-variables that are positive and re-solve until e-LP is infeasible
  HighsInt loop_k = 0;
  bool feasible_model = false;
  for (;;) {
    if (kIisDevReport)
      printf("\nElasticity filter pass %d\n==============\n", int(loop_k));
    HighsInt num_fixed = 0;
    if (has_elastic_columns) {
      for (size_t eCol = 0; eCol < col_of_ecol.size(); eCol++) {
        HighsInt iCol = col_of_ecol[eCol];
        if (solution.col_value[col_ecol_offset + eCol] >
            this->options_.primal_feasibility_tolerance) {
          if (kIisDevReport)
            printf(
                "E-col %2d (column %2d) corresponds to column %2d with bound "
                "%g "
                "and has solution value %g\n",
                int(eCol), int(col_ecol_offset + eCol), int(iCol),
                bound_of_col_of_ecol[eCol],
                solution.col_value[col_ecol_offset + eCol]);
          this->changeColBounds(col_ecol_offset + eCol, 0, 0);
          num_fixed++;
        }
      }
    }
    if (has_elastic_rows) {
      for (size_t eCol = 0; eCol < row_of_ecol.size(); eCol++) {
        HighsInt iRow = row_of_ecol[eCol];
        if (solution.col_value[row_ecol_offset + eCol] >
            this->options_.primal_feasibility_tolerance) {
          if (kIisDevReport)
            printf(
                "E-row %2d (column %2d) corresponds to    row %2d with bound "
                "%g "
                "and has solution value %g\n",
                int(eCol), int(row_ecol_offset + eCol), int(iRow),
                bound_of_row_of_ecol[eCol],
                solution.col_value[row_ecol_offset + eCol]);
          this->changeColBounds(row_ecol_offset + eCol, 0, 0);
          num_fixed++;
        }
      }
    }
    if (num_fixed == 0) {
      // No elastic variables were positive, so problem is feasible
      feasible_model = true;
      break;
    }
    HighsStatus run_status = solveLp();
    if (run_status != HighsStatus::kOk)
      return elasticityFilterReturn(run_status, feasible_model,
                                    original_num_col, original_num_row,
                                    original_col_cost, original_col_lower,
                                    original_col_upper, original_integrality);
    if (kIisDevReport) this->writeSolution("", kSolutionStylePretty);
    HighsModelStatus model_status = this->getModelStatus();
    if (model_status == HighsModelStatus::kInfeasible) break;
    loop_k++;
  }

  infeasible_row_subset.clear();
  HighsInt num_enforced_col_ecol = 0;
  HighsInt num_enforced_row_ecol = 0;
  if (has_elastic_columns) {
    for (size_t eCol = 0; eCol < col_of_ecol.size(); eCol++) {
      HighsInt iCol = col_of_ecol[eCol];
      if (lp.col_upper_[col_ecol_offset + eCol] == 0) {
        num_enforced_col_ecol++;
        printf(
            "Col e-col %2d (column %2d) corresponds to column %2d with bound "
            "%g "
            "and is enforced\n",
            int(eCol), int(col_ecol_offset + eCol), int(iCol),
            bound_of_col_of_ecol[eCol]);
      }
    }
  }
  if (has_elastic_rows) {
    for (size_t eCol = 0; eCol < row_of_ecol.size(); eCol++) {
      HighsInt iRow = row_of_ecol[eCol];
      if (lp.col_upper_[row_ecol_offset + eCol] == 0) {
        num_enforced_row_ecol++;
        infeasible_row_subset.push_back(iRow);
        if (kIisDevReport)
          printf(
              "Row e-col %2d (column %2d) corresponds to    row %2d with bound "
              "%g and is enforced\n",
              int(eCol), int(row_ecol_offset + eCol), int(iRow),
              bound_of_row_of_ecol[eCol]);
      }
    }
  }
  if (feasible_model)
    assert(num_enforced_col_ecol == 0 && num_enforced_row_ecol == 0);

  highsLogUser(
      options_.log_options, HighsLogType::kInfo,
      "Elasticity filter after %d passes enforces bounds on %d cols and %d "
      "rows\n",
      int(loop_k), int(num_enforced_col_ecol), int(num_enforced_row_ecol));

  if (kIisDevReport)
    printf(
        "\nElasticity filter after %d passes enforces bounds on %d cols and %d "
        "rows\n",
        int(loop_k), int(num_enforced_col_ecol), int(num_enforced_row_ecol));

  return elasticityFilterReturn(HighsStatus::kOk, feasible_model,
                                original_num_col, original_num_row,
                                original_col_cost, original_col_lower,
                                original_col_upper, original_integrality);
}

HighsStatus Highs::extractIis(HighsInt& num_iis_col, HighsInt& num_iis_row,
                              HighsInt* iis_col_index, HighsInt* iis_row_index,
                              HighsInt* iis_col_bound,
                              HighsInt* iis_row_bound) {
  assert(this->iis_.valid_);
  num_iis_col = this->iis_.col_index_.size();
  num_iis_row = this->iis_.row_index_.size();
  if (iis_col_index || iis_col_bound) {
    for (HighsInt iCol = 0; iCol < num_iis_col; iCol++) {
      if (iis_col_index) iis_col_index[iCol] = this->iis_.col_index_[iCol];
      if (iis_col_bound) iis_col_bound[iCol] = this->iis_.col_bound_[iCol];
    }
  }
  if (iis_row_index || iis_row_bound) {
    for (HighsInt iRow = 0; iRow < num_iis_row; iRow++) {
      if (iis_row_index) iis_row_index[iRow] = this->iis_.row_index_[iRow];
      if (iis_row_bound) iis_row_bound[iRow] = this->iis_.row_bound_[iRow];
    }
  }
  return HighsStatus::kOk;
}

bool Highs::aFormatOk(const HighsInt num_nz, const HighsInt format) {
  if (!num_nz) return true;
  const bool ok_format = format == (HighsInt)MatrixFormat::kColwise ||
                         format == (HighsInt)MatrixFormat::kRowwise;
  assert(ok_format);
  if (!ok_format)
    highsLogUser(
        options_.log_options, HighsLogType::kError,
        "Non-empty Constraint matrix has illegal format = %" HIGHSINT_FORMAT
        "\n",
        format);
  return ok_format;
}

bool Highs::qFormatOk(const HighsInt num_nz, const HighsInt format) {
  if (!num_nz) return true;
  const bool ok_format = format == (HighsInt)HessianFormat::kTriangular;
  assert(ok_format);
  if (!ok_format)
    highsLogUser(
        options_.log_options, HighsLogType::kError,
        "Non-empty Hessian matrix has illegal format = %" HIGHSINT_FORMAT "\n",
        format);
  return ok_format;
}

void Highs::clearZeroHessian() {
  HighsHessian& hessian = model_.hessian_;
  if (hessian.dim_) {
    // Clear any zero Hessian
    if (hessian.numNz() == 0) {
      highsLogUser(options_.log_options, HighsLogType::kInfo,
                   "Hessian has dimension %" HIGHSINT_FORMAT
                   " but no nonzeros, so is ignored\n",
                   hessian.dim_);
      hessian.clear();
    }
  }
}

HighsStatus Highs::checkOptimality(const std::string& solver_type) {
  // Check for infeasibility measures incompatible with optimality
  assert(model_status_ == HighsModelStatus::kOptimal);
  // Cannot expect to have no dual_infeasibilities since the QP solver
  // (and, of course, the MIP solver) give no dual information
  if (info_.num_primal_infeasibilities == 0 &&
      info_.num_dual_infeasibilities <= 0)
    return HighsStatus::kOk;
  model_status_ = HighsModelStatus::kSolveError;
  std::stringstream ss;
  ss.str(std::string());
  ss << highsFormatToString(
      "%s solver claims optimality, but with num/max/sum "
      "primal(%d/%g/%g)",
      solver_type.c_str(), int(info_.num_primal_infeasibilities),
      info_.max_primal_infeasibility, info_.sum_primal_infeasibilities);
  if (info_.num_dual_infeasibilities > 0)
    ss << highsFormatToString(
        "and dual(%d/%g/%g)", int(info_.num_dual_infeasibilities),
        info_.max_dual_infeasibility, info_.sum_dual_infeasibilities);
  ss << " infeasibilities\n";
  const std::string report_string = ss.str();
  highsLogUser(options_.log_options, HighsLogType::kError, "%s",
               report_string.c_str());
  highsLogUser(options_.log_options, HighsLogType::kError,
               "Setting model status to %s\n",
               modelStatusToString(model_status_).c_str());
  return HighsStatus::kError;
}

HighsStatus Highs::lpKktCheck(const std::string& message) {
  if (!this->solution_.value_valid) return HighsStatus::kOk;
  // Must have dual values for an LP if there are primal values
  assert(this->solution_.dual_valid);
  HighsInfo& info = this->info_;
  const HighsOptions& options = this->options_;
  const HighsSolution& solution = this->solution_;
  const HighsLogOptions& log_options = options.log_options;
  double primal_feasibility_tolerance = options.primal_feasibility_tolerance;
  double dual_feasibility_tolerance = options.dual_feasibility_tolerance;
  double primal_residual_tolerance = options.primal_residual_tolerance;
  double dual_residual_tolerance = options.dual_residual_tolerance;
  double optimality_tolerance = options.optimality_tolerance;
  if (options.kkt_tolerance != kDefaultKktTolerance) {
    primal_feasibility_tolerance = options.kkt_tolerance;
    dual_feasibility_tolerance = options.kkt_tolerance;
    primal_residual_tolerance = options.kkt_tolerance;
    dual_residual_tolerance = options.kkt_tolerance;
    optimality_tolerance = options.kkt_tolerance;
  }
  info.objective_function_value =
      model_.lp_.objectiveValue(solution_.col_value);
  HighsPrimalDualErrors primal_dual_errors;
  const bool get_residuals = !basis_.valid;
  getLpKktFailures(options, model_.lp_, solution, basis_, info,
                   primal_dual_errors, get_residuals);
  //  highsLogUser(options.log_options, HighsLogType::kInfo,
  //               "Highs::lpKktCheck: %s\n", message.c_str());
  if (this->model_status_ == HighsModelStatus::kOptimal)
    reportLpKktFailures(model_.lp_, options, info, "LP");
  // get_residuals is false when there is a valid basis, since
  // residual errors are assumed to be small, so
  // info.num_primal_residual_errors = -1, since they aren't
  // known. Hence don't consider this in identifying unboundedness
  // from HighsModelStatus::kUnboundedOrInfeasible
  if (model_status_ == HighsModelStatus::kUnboundedOrInfeasible &&
      info.num_primal_infeasibilities == 0 &&
      (!get_residuals || info.num_primal_residual_errors == 0))
    model_status_ = HighsModelStatus::kUnbounded;
  bool was_optimal = model_status_ == HighsModelStatus::kOptimal;
  bool kkt_ok = true;
  bool written_optimality_error_header = false;

  auto foundOptimalityError = [&]() {
    kkt_ok = false;
    if (!was_optimal || written_optimality_error_header) return;
    highsLogUser(log_options, HighsLogType::kWarning,
                 "LP solver claims optimality, but with\n");
    written_optimality_error_header = true;
  };

  double max_primal_tolerance_relative_violation = 0;
  double max_dual_tolerance_relative_violation = 0;
  double primal_dual_objective_tolerance_relative_violation = 0;
  const double max_allowed_tolerance_relative_violation = 1e2;
  if (basis_.valid) {
    if (info.num_primal_infeasibilities > 0) {
      max_primal_tolerance_relative_violation =
          std::max(info.max_primal_infeasibility / primal_feasibility_tolerance,
                   max_primal_tolerance_relative_violation);
      foundOptimalityError();
      if (was_optimal)
        highsLogUser(
            log_options, HighsLogType::kWarning,
            "   num/max/sum %6d / %8.3g / %8.3g primal "
            "infeasibilities       (tolerance = %4.0e)\n",
            int(info.num_primal_infeasibilities), info.max_primal_infeasibility,
            info.sum_primal_infeasibilities, primal_feasibility_tolerance);
    }
    if (info.num_dual_infeasibilities > 0) {
      max_dual_tolerance_relative_violation =
          std::max(info.max_dual_infeasibility / dual_feasibility_tolerance,
                   max_dual_tolerance_relative_violation);
      foundOptimalityError();
      if (was_optimal)
        highsLogUser(log_options, HighsLogType::kWarning,
                     "   num/max/sum %6d / %8.3g / %8.3g   dual "
                     "infeasibilities       (tolerance = %4.0e)\n",
                     int(info.num_dual_infeasibilities),
                     info.max_dual_infeasibility, info.sum_dual_infeasibilities,
                     dual_feasibility_tolerance);
    }
    // An optimal basic solution has no complementarity violations
    // by construction, and can be assumed to have no relative
    // primal or dual residual errors or meaningful primal dual
    // objective error
    bool unexpected_error_if_optimal = info.num_complementarity_violations != 0;
    double local_dual_objective = 0;
    if (info.primal_dual_objective_error > optimality_tolerance) {
      // Ignore primal-dual objective errors if both objectives are small
      const bool ok_dual_objective = computeDualObjectiveValue(
          nullptr, this->model_.lp_, this->solution_, local_dual_objective);
      assert(ok_dual_objective);
      if (info.objective_function_value * info.objective_function_value >
              optimality_tolerance &&
          local_dual_objective * local_dual_objective > optimality_tolerance)
        unexpected_error_if_optimal = true;
    }
    const bool have_residual_errors =
        info.num_primal_residual_errors != kHighsIllegalResidualCount;
    if (have_residual_errors) {
      unexpected_error_if_optimal =
          unexpected_error_if_optimal ||
          info.num_relative_primal_residual_errors != 0 ||
          info.num_relative_dual_residual_errors != 0;
      max_primal_tolerance_relative_violation = std::max(
          info.max_relative_primal_residual_error / primal_residual_tolerance,
          max_primal_tolerance_relative_violation);
      max_dual_tolerance_relative_violation = std::max(
          info.max_relative_dual_residual_error / dual_residual_tolerance,
          max_dual_tolerance_relative_violation);
    }
    primal_dual_objective_tolerance_relative_violation =
        info.primal_dual_objective_error / optimality_tolerance;

    if (was_optimal && unexpected_error_if_optimal) {
      highsLogUser(
          log_options, HighsLogType::kWarning,
          "Optimal basic solution has %d complementarity violations and %g "
          "primal dual objective error from primal (dual) objective = %g "
          "(%g)\n",
          int(info.num_complementarity_violations),
          info.primal_dual_objective_error, info.objective_function_value,
          local_dual_objective);
      if (have_residual_errors) {
        highsLogUser(
            log_options, HighsLogType::kWarning,
            "   num/max %6d / %8.3g  relative primal residual errors         "
            "(tolerance = %4.0e)\n",
            int(info.num_relative_primal_residual_errors),
            info.max_relative_primal_residual_error, primal_residual_tolerance);
        highsLogUser(
            log_options, HighsLogType::kWarning,
            "   num/max %6d / %8.3g  relative   dual residual errors         "
            "(tolerance = %4.0e)\n",
            int(info.num_relative_dual_residual_errors),
            info.max_relative_dual_residual_error, dual_residual_tolerance);
      }
      assert(info.num_complementarity_violations == 0);
      assert(info.primal_dual_objective_error <= optimality_tolerance);
      if (have_residual_errors) {
        assert(info.num_relative_primal_residual_errors == 0);
        assert(info.num_relative_dual_residual_errors == 0);
      }
    }
    // Infeasibility of the primal and dual solutions based on number
    // of primal/dual infeasibilities should have been set in
    // getKktFailures, but qualify this if the residuals are
    // meaningful
    if (info.num_primal_infeasibilities) {
      assert(info.primal_solution_status == kSolutionStatusInfeasible);
    } else {
      info.primal_solution_status = kSolutionStatusFeasible;
    }
    if (info.num_dual_infeasibilities) {
      assert(info.dual_solution_status == kSolutionStatusInfeasible);
    } else {
      info.dual_solution_status = kSolutionStatusFeasible;
    }
    // Overrule feasibility if large relative tolerance failures have
    // ocurred - pretty inconceivable since absolute residuals should
    // be small with a basis
    if (max_primal_tolerance_relative_violation >
        max_allowed_tolerance_relative_violation)
      info.primal_solution_status = kSolutionStatusInfeasible;
    if (max_dual_tolerance_relative_violation >
        max_allowed_tolerance_relative_violation)
      info.dual_solution_status = kSolutionStatusInfeasible;
  } else {
    // A solution without a basis may have primal or dual residual
    // errors, and complementarity errors - due to the convergence
    // being based on relative primal-dual objective error, so test
    // the latter
    double tolerance_relative_violation =
        info.max_relative_primal_infeasibility / primal_feasibility_tolerance;
    max_primal_tolerance_relative_violation = std::max(
        tolerance_relative_violation, max_primal_tolerance_relative_violation);
    if (info.num_relative_primal_infeasibilities > 0) {
      foundOptimalityError();
      if (was_optimal)
        highsLogUser(log_options, HighsLogType::kWarning,
                     "   num/max %6d / %8.3g relative primal infeasibilities "
                     "(tolerance = %4.0e)\n",
                     int(info.num_relative_primal_infeasibilities),
                     info.max_relative_primal_infeasibility,
                     primal_feasibility_tolerance);
    }
    tolerance_relative_violation =
        info.max_relative_dual_infeasibility / dual_feasibility_tolerance;
    max_dual_tolerance_relative_violation = std::max(
        tolerance_relative_violation, max_dual_tolerance_relative_violation);
    if (info.num_relative_dual_infeasibilities > 0) {
      foundOptimalityError();
      if (was_optimal)
        highsLogUser(log_options, HighsLogType::kWarning,
                     "   num/max %6d / %8.3g relative   dual infeasibilities "
                     "(tolerance = %4.0e)\n",
                     int(info.num_relative_dual_infeasibilities),
                     info.max_relative_dual_infeasibility,
                     dual_feasibility_tolerance);
    }
    tolerance_relative_violation =
        info.max_relative_primal_residual_error / primal_residual_tolerance;
    max_primal_tolerance_relative_violation = std::max(
        tolerance_relative_violation, max_primal_tolerance_relative_violation);
    if (info.num_relative_primal_residual_errors > 0) {
      foundOptimalityError();
      if (was_optimal)
        highsLogUser(log_options, HighsLogType::kWarning,
                     "   num/max %6d / %8.3g relative primal residual errors "
                     "(tolerance = %4.0e)\n",
                     int(info.num_relative_primal_residual_errors),
                     info.max_relative_primal_residual_error,
                     primal_residual_tolerance);
    }
    tolerance_relative_violation =
        info.max_relative_dual_residual_error / dual_residual_tolerance;
    max_dual_tolerance_relative_violation = std::max(
        tolerance_relative_violation, max_dual_tolerance_relative_violation);
    if (info.num_relative_dual_residual_errors > 0) {
      foundOptimalityError();
      if (was_optimal)
        highsLogUser(log_options, HighsLogType::kWarning,
                     "   num/max %6d / %8.3g relative   dual residual errors "
                     "(tolerance = %4.0e)\n",
                     int(info.num_relative_dual_residual_errors),
                     info.max_relative_dual_residual_error,
                     dual_residual_tolerance);
    }
    if (info.primal_dual_objective_error > optimality_tolerance) {
      primal_dual_objective_tolerance_relative_violation =
          info.primal_dual_objective_error / optimality_tolerance;
      foundOptimalityError();
      if (was_optimal)
        highsLogUser(log_options, HighsLogType::kWarning,
                     "                 %8.3g relative P-D objective error    "
                     "(tolerance = %4.0e)\n",
                     info.primal_dual_objective_error, optimality_tolerance);
    }
    // Set the primal and dual solution status according to tolerance failure
    if (max_primal_tolerance_relative_violation >
        max_allowed_tolerance_relative_violation) {
      info.primal_solution_status = kSolutionStatusInfeasible;
    } else {
      info.primal_solution_status = kSolutionStatusFeasible;
    }
    if (max_dual_tolerance_relative_violation >
        max_allowed_tolerance_relative_violation) {
      info.dual_solution_status = kSolutionStatusInfeasible;
    } else {
      info.dual_solution_status = kSolutionStatusFeasible;
    }
  }
  double max_tolerance_relative_violation =
      primal_dual_objective_tolerance_relative_violation;
  max_tolerance_relative_violation =
      std::max(max_primal_tolerance_relative_violation,
               max_tolerance_relative_violation);
  max_tolerance_relative_violation = std::max(
      max_dual_tolerance_relative_violation, max_tolerance_relative_violation);
  //
  // Now see whether optimality is compromised or permitted given the tolerance
  // failures
  if (model_status_ == HighsModelStatus::kOptimal) {
    if (max_tolerance_relative_violation >
        max_allowed_tolerance_relative_violation) {
      model_status_ = HighsModelStatus::kUnknown;
      highsLogUser(log_options, HighsLogType::kWarning,
                   "Model status changed from \"Optimal\" to \"Unknown\""
                   " since relative violation of tolerances is %8.3g\n",
                   max_tolerance_relative_violation);
    } else if (max_allowed_tolerance_relative_violation > 1 &&
               max_tolerance_relative_violation > 1) {
      highsLogUser(log_options, HighsLogType::kInfo,
                   "Model status is \"Optimal\" since relative violation of "
                   "tolerances is no more than %8.3g\n",
                   max_tolerance_relative_violation);
    }
  } else if (model_status_ == HighsModelStatus::kUnknown &&
             max_tolerance_relative_violation <=
                 max_allowed_tolerance_relative_violation) {
    model_status_ = HighsModelStatus::kOptimal;
    highsLogUser(log_options, HighsLogType::kWarning,
                 "Model status changed from \"Unknown\" to \"Optimal\"\n");
  }
  return HighsStatus::kOk;
}

HighsStatus Highs::invertRequirementError(std::string method_name) const {
  assert(!ekk_instance_.status_.has_invert);
  highsLogUser(options_.log_options, HighsLogType::kError,
               "No invertible representation for %s\n", method_name.c_str());
  return HighsStatus::kError;
}

HighsStatus Highs::handleInfCost() {
  HighsLp& lp = this->model_.lp_;
  if (!lp.has_infinite_cost_) return HighsStatus::kOk;
  HighsLpMods& mods = lp.mods_;
  double inf_cost = this->options_.infinite_cost;
  for (HighsInt k = 0; k < 2; k++) {
    // Pass twice: first checking that infinite costs can be handled,
    // then handling them, so that model is unmodified if infinite
    // costs cannot be handled
    for (HighsInt iCol = 0; iCol < lp.num_col_; iCol++) {
      double cost = lp.col_cost_[iCol];
      if (cost > -inf_cost && cost < inf_cost) continue;
      double lower = lp.col_lower_[iCol];
      double upper = lp.col_upper_[iCol];
      if (lp.isMip()) {
        if (lp.integrality_[iCol] == HighsVarType::kInteger) {
          lower = std::ceil(lower);
          upper = std::floor(upper);
        }
      }
      if (cost <= -inf_cost) {
        if (lp.sense_ == ObjSense::kMinimize) {
          // Minimizing with -inf cost so try to fix at upper bound
          if (upper < kHighsInf) {
            if (k) lp.col_lower_[iCol] = upper;
          } else {
            highsLogUser(options_.log_options, HighsLogType::kError,
                         "Cannot minimize with a cost on variable %d of %g and "
                         "upper bound of %g\n",
                         int(iCol), cost, upper);
            return HighsStatus::kError;
          }
        } else {
          // Maximizing with -inf cost so try to fix at lower bound
          if (lower > -kHighsInf) {
            if (k) lp.col_upper_[iCol] = lower;
          } else {
            highsLogUser(options_.log_options, HighsLogType::kError,
                         "Cannot maximize with a cost on variable %d of %g and "
                         "lower bound of %g\n",
                         int(iCol), cost, lower);
            return HighsStatus::kError;
          }
        }
      } else {
        if (lp.sense_ == ObjSense::kMinimize) {
          // Minimizing with inf cost so try to fix at lower bound
          if (lower > -kHighsInf) {
            if (k) lp.col_upper_[iCol] = lower;
          } else {
            highsLogUser(options_.log_options, HighsLogType::kError,
                         "Cannot minimize with a cost on variable %d of %g and "
                         "lower bound of %g\n",
                         int(iCol), cost, lower);
            return HighsStatus::kError;
          }
        } else {
          // Maximizing with inf cost so try to fix at upper bound
          if (upper < kHighsInf) {
            if (k) lp.col_lower_[iCol] = upper;
          } else {
            highsLogUser(options_.log_options, HighsLogType::kError,
                         "Cannot maximize with a cost on variable %d of %g and "
                         "upper bound of %g\n",
                         int(iCol), cost, upper);
            return HighsStatus::kError;
          }
        }
      }
      if (k) {
        mods.save_inf_cost_variable_index.push_back(iCol);
        mods.save_inf_cost_variable_cost.push_back(cost);
        mods.save_inf_cost_variable_lower.push_back(lower);
        mods.save_inf_cost_variable_upper.push_back(upper);
        lp.col_cost_[iCol] = 0;
      }
    }
  }
  // Infinite costs have been removed, but their presence in the
  // original model is known from mods.save_inf_cost_variable_*, so
  // set lp.has_infinite_cost_ to be false to avoid assert when run()
  // is called using copy of model in MIP solver (See #1446)
  lp.has_infinite_cost_ = false;

  return HighsStatus::kOk;
}

void Highs::restoreInfCost(HighsStatus& return_status) {
  HighsLp& lp = this->model_.lp_;
  HighsBasis& basis = this->basis_;
  HighsLpMods& mods = lp.mods_;
  HighsInt num_inf_cost = mods.save_inf_cost_variable_index.size();
  if (num_inf_cost <= 0) return;
  assert(num_inf_cost);
  for (HighsInt ix = 0; ix < num_inf_cost; ix++) {
    HighsInt iCol = mods.save_inf_cost_variable_index[ix];
    double cost = mods.save_inf_cost_variable_cost[ix];
    double lower = mods.save_inf_cost_variable_lower[ix];
    double upper = mods.save_inf_cost_variable_upper[ix];
    double value = solution_.value_valid ? solution_.col_value[iCol] : 0;
    if (basis.valid) {
      assert(basis.col_status[iCol] != HighsBasisStatus::kBasic);
      if (lp.col_lower_[iCol] == lower) {
        basis.col_status[iCol] = HighsBasisStatus::kLower;
      } else {
        basis.col_status[iCol] = HighsBasisStatus::kUpper;
      }
    }
    assert(lp.col_cost_[iCol] == 0);
    if (value) this->info_.objective_function_value += value * cost;
    lp.col_cost_[iCol] = cost;
    lp.col_lower_[iCol] = lower;
    lp.col_upper_[iCol] = upper;
  }
  // Infinite costs have been reintroduced, so reset to true the flag
  // that was set false in Highs::handleInfCost() (See #1446)
  lp.has_infinite_cost_ = true;

  if (this->model_status_ == HighsModelStatus::kInfeasible) {
    // Model is infeasible with the infinite cost variables fixed at
    // appropriate values, so model status cannot be determined
    this->model_status_ = HighsModelStatus::kUnknown;
    setHighsModelStatusAndClearSolutionAndBasis(this->model_status_);
    return_status = highsStatusFromHighsModelStatus(model_status_);
  }
}

// Modify status and info if user bound or cost scaling, or
// primal/dual feasibility tolerances have changed
HighsStatus Highs::optionChangeAction() {
  HighsModel& model = this->model_;
  HighsLp& lp = model.lp_;
  HighsInfo& info = this->info_;
  HighsOptions& options = this->options_;
  const bool is_mip = lp.isMip();
  HighsInt dl_user_bound_scale = 0;
  double dl_user_bound_scale_value = 1;
  // Ensure that user bound scaling does not yield infinite bounds
  const bool changed_user_bound_scale =
      options.user_bound_scale != lp.user_bound_scale_;
  bool user_bound_scale_ok =
      !changed_user_bound_scale ||
      lp.userBoundScaleOk(options.user_bound_scale, options.infinite_bound);
  if (!user_bound_scale_ok) {
    options.user_bound_scale = lp.user_bound_scale_;
    highsLogUser(options_.log_options, HighsLogType::kError,
                 "New user bound scaling yields infinite bound: reverting user "
                 "bound scaling to %d\n",
                 int(options.user_bound_scale));
  } else if (changed_user_bound_scale) {
    dl_user_bound_scale = options.user_bound_scale - lp.user_bound_scale_;
    dl_user_bound_scale_value = std::pow(2, dl_user_bound_scale);
  }
  // Now consider impact on primal feasibility of user bound scaling
  // and/or primal_feasibility_tolerance change.
  //
  double new_max_primal_infeasibility =
      info.max_primal_infeasibility * dl_user_bound_scale_value;
  if (new_max_primal_infeasibility > options.primal_feasibility_tolerance) {
    // Not primal feasible: only act if the model is currently primal
    // feasible or dl_user_bound_scale_value > 1
    if (info.num_primal_infeasibilities == 0 && dl_user_bound_scale_value > 1) {
      this->model_status_ = HighsModelStatus::kNotset;
      if (info.primal_solution_status == kSolutionStatusFeasible)
        highsLogUser(options_.log_options, HighsLogType::kInfo,
                     "Option change leads to loss of primal feasibility\n");
      info.primal_solution_status = kSolutionStatusInfeasible;
      info.num_primal_infeasibilities = kHighsIllegalInfeasibilityCount;
    }
  } else if (!is_mip &&
             info.primal_solution_status == kSolutionStatusInfeasible) {
    highsLogUser(options_.log_options, HighsLogType::kInfo,
                 "Option change leads to gain of primal feasibility\n");
    info.primal_solution_status = kSolutionStatusFeasible;
    info.num_primal_infeasibilities = 0;
  }
  if (is_mip && dl_user_bound_scale) {
    // MIP with non-trivial bound scaling loses optimality
    this->model_status_ = HighsModelStatus::kNotset;
    if (dl_user_bound_scale < 0) {
      // MIP with negative bound scaling exponent loses feasibility
      if (info.primal_solution_status == kSolutionStatusFeasible) {
        highsLogUser(
            options_.log_options, HighsLogType::kInfo,
            "Option change leads to loss of primal feasibility for MIP\n");
      }
      info.primal_solution_status = kSolutionStatusInfeasible;
    }
  }
  if (dl_user_bound_scale) {
    // Update info and solution with respect to non-trivial user bound
    // scaling
    //
    // max and sum of infeasibilities scales: num is handled later
    info.objective_function_value *= dl_user_bound_scale_value;
    info.max_primal_infeasibility *= dl_user_bound_scale_value;
    info.sum_primal_infeasibilities *= dl_user_bound_scale_value;
    for (HighsInt iCol = 0; iCol < lp.num_col_; iCol++)
      this->solution_.col_value[iCol] *= dl_user_bound_scale_value;
    for (HighsInt iRow = 0; iRow < lp.num_row_; iRow++)
      this->solution_.row_value[iRow] *= dl_user_bound_scale_value;
    // Update LP with respect to non-trivial user bound scaling
    lp.userBoundScale(options_.user_bound_scale);
  }
  // Now consider whether options.user_cost_scale has changed
  HighsInt dl_user_cost_scale = 0;
  double dl_user_cost_scale_value = 1;
  const bool changed_user_cost_scale =
      options.user_cost_scale != lp.user_cost_scale_;
  bool user_cost_scale_ok =
      !changed_user_cost_scale ||
      model.userCostScaleOk(options.user_cost_scale, options.small_matrix_value,
                            options.large_matrix_value, options.infinite_cost);
  if (!user_cost_scale_ok) {
    options.user_cost_scale = lp.user_cost_scale_;
    highsLogUser(options_.log_options, HighsLogType::kError,
                 "New user cost scaling yields excessive cost coefficient: "
                 "reverting user cost scaling to %d\n",
                 int(options.user_cost_scale));
  } else if (changed_user_cost_scale) {
    dl_user_cost_scale = options.user_cost_scale - lp.user_cost_scale_;
    dl_user_cost_scale_value = std::pow(2, dl_user_cost_scale);
  }
  if (!is_mip) {
    // Now consider impact on dual feasibility of user cost scaling
    // and/or dual_feasibility_tolerance change
    double new_max_dual_infeasibility =
        info.max_dual_infeasibility * dl_user_cost_scale_value;
    if (new_max_dual_infeasibility > options.dual_feasibility_tolerance) {
      // Not dual feasible: only act if the model is currently dual
      // feasible or dl_user_bound_scale_value > 1
      if (info.num_dual_infeasibilities == 0 && dl_user_cost_scale_value > 1) {
        this->model_status_ = HighsModelStatus::kNotset;
        if (info.dual_solution_status == kSolutionStatusFeasible) {
          highsLogUser(options_.log_options, HighsLogType::kInfo,
                       "Option change leads to loss of dual feasibility\n");
          info.dual_solution_status = kSolutionStatusInfeasible;
        }
        info.num_dual_infeasibilities = kHighsIllegalInfeasibilityCount;
      }
    } else if (info.dual_solution_status == kSolutionStatusInfeasible) {
      highsLogUser(options_.log_options, HighsLogType::kInfo,
                   "Option change leads to gain of dual feasibility\n");
      info.dual_solution_status = kSolutionStatusFeasible;
      info.num_dual_infeasibilities = 0;
    }
  }
  if (dl_user_cost_scale) {
    if (is_mip) {
      // MIP with non-trivial cost scaling loses optimality
      this->model_status_ = HighsModelStatus::kNotset;
    }
    // Now update data and solution with respect to non-trivial user
    // cost scaling
    //
    // max and sum of infeasibilities scales: num is handled earlier
    info.objective_function_value *= dl_user_cost_scale_value;
    info.max_dual_infeasibility *= dl_user_cost_scale_value;
    info.sum_dual_infeasibilities *= dl_user_cost_scale_value;
    for (HighsInt iCol = 0; iCol < lp.num_col_; iCol++)
      this->solution_.col_dual[iCol] *= dl_user_cost_scale_value;
    for (HighsInt iRow = 0; iRow < lp.num_row_; iRow++)
      this->solution_.row_dual[iRow] *= dl_user_cost_scale_value;
    model.userCostScale(options.user_cost_scale);
  }
  // Too hard to identify optimality from primal/dual solution status,
  // since (for example) after IPX without crossover on an infeasible
  // LP, primal/dual solution status may be feasible, but there are
  // primal/dual residual errors. There could also be complementarity
  // errors, even at a feasible point
  //
  /*
  if (this->model_status_ != HighsModelStatus::kOptimal) {
    if (info.primal_solution_status == kSolutionStatusFeasible &&
        info.dual_solution_status == kSolutionStatusFeasible) {
      highsLogUser(options_.log_options, HighsLogType::kInfo,
                   "Option change leads to gain of optimality\n");
      this->model_status_ = HighsModelStatus::kOptimal;
    }
  }
  */
  if (!user_bound_scale_ok || !user_cost_scale_ok) return HighsStatus::kError;
  if (this->iis_.valid_ && options_.iis_strategy != this->iis_.strategy_)
    this->iis_.invalidate();
  return HighsStatus::kOk;
}

void HighsIllConditioning::clear() { this->record.clear(); }

HighsStatus Highs::computeIllConditioning(
    HighsIllConditioning& ill_conditioning, const bool constraint,
    const HighsInt method, const double ill_conditioning_bound) {
  const double kZeroMultiplier = 1e-6;
  ill_conditioning.clear();
  HighsLp& incumbent_lp = this->model_.lp_;
  Highs conditioning;
  const bool dev_conditioning = false;
  conditioning.setOptionValue("output_flag", false);  // dev_conditioning);
  std::vector<HighsInt> basic_var;
  HighsLp& ill_conditioning_lp = conditioning.model_.lp_;
  // Form the ill-conditioning LP according to method
  if (method == 0) {
    formIllConditioningLp0(ill_conditioning_lp, basic_var, constraint);
  } else {
    formIllConditioningLp1(ill_conditioning_lp, basic_var, constraint,
                           ill_conditioning_bound);
    //    conditioning.writeModel("");
  }

  //  if (dev_conditioning) conditioning.writeModel("");

  assert(assessLp(ill_conditioning_lp, this->options_) == HighsStatus::kOk);
  // Solve the ill-conditioning analysis LP
  HighsStatus return_status = conditioning.run();
  HighsModelStatus model_status = conditioning.getModelStatus();
  const std::string type = constraint ? "Constraint" : "Column";
  const bool failed =
      return_status != HighsStatus::kOk ||
      (method == 0 && model_status != HighsModelStatus::kOptimal) ||
      (method == 1 && (model_status != HighsModelStatus::kOptimal &&
                       model_status != HighsModelStatus::kInfeasible));
  if (failed) {
    highsLogUser(options_.log_options, HighsLogType::kInfo,
                 "\n%s view ill-conditioning analysis has failed\n",
                 type.c_str());
    return HighsStatus::kError;
  }
  if (method == 1 && model_status == HighsModelStatus::kInfeasible) {
    highsLogUser(options_.log_options, HighsLogType::kInfo,
                 "\n%s view ill-conditioning bound of %g is insufficient for "
                 "analysis: try %g\n",
                 type.c_str(), ill_conditioning_bound,
                 1e1 * ill_conditioning_bound);
    return HighsStatus::kOk;
  }
  if (dev_conditioning) conditioning.writeSolution("", 1);
  // Extract and normalise the multipliers
  HighsSolution& solution = conditioning.solution_;
  double multiplier_norm = 0;
  for (HighsInt iRow = 0; iRow < incumbent_lp.num_row_; iRow++)
    multiplier_norm += std::fabs(solution.col_value[iRow]);
  assert(multiplier_norm > 0);
  const double ill_conditioning_measure =
      (method == 0 ? conditioning.getInfo().objective_function_value
                   : solution.row_value[conditioning.getNumRow() - 1]) /
      multiplier_norm;
  highsLogUser(
      options_.log_options, HighsLogType::kInfo,
      "\n%s view ill-conditioning analysis: 1-norm distance of basis matrix "
      "from singularity is estimated to be %g\n",
      type.c_str(), ill_conditioning_measure);
  std::vector<std::pair<double, HighsInt>> abs_list;
  for (HighsInt iRow = 0; iRow < incumbent_lp.num_row_; iRow++) {
    double abs_multiplier =
        std::fabs(solution.col_value[iRow]) / multiplier_norm;
    if (abs_multiplier <= kZeroMultiplier) continue;
    abs_list.push_back(std::make_pair(abs_multiplier, iRow));
  }
  std::sort(abs_list.begin(), abs_list.end());
  // Report on ill-conditioning multipliers
  std::stringstream ss;
  const bool has_row_names =
      HighsInt(incumbent_lp.row_names_.size()) == incumbent_lp.num_row_;
  const bool has_col_names =
      HighsInt(incumbent_lp.col_names_.size()) == incumbent_lp.num_col_;
  const double coefficient_zero_tolerance = 1e-8;
  auto printCoefficient = [&](const double multiplier, const bool first) {
    if (std::fabs(multiplier) < coefficient_zero_tolerance) {
      ss << "+ 0";
    } else if (std::fabs(multiplier - 1) < coefficient_zero_tolerance) {
      std::string str = first ? "" : "+ ";
      ss << str;
    } else if (std::fabs(multiplier + 1) < coefficient_zero_tolerance) {
      std::string str = first ? "-" : "- ";
      ss << str;
    } else if (multiplier < 0) {
      std::string str = first ? "-" : "- ";
      ss << str << -multiplier << " ";
    } else {
      std::string str = first ? "" : "+ ";
      ss << str << multiplier << " ";
    }
  };

  for (HighsInt iX = int(abs_list.size()) - 1; iX >= 0; iX--) {
    HighsInt iRow = abs_list[iX].second;
    HighsIllConditioningRecord record;
    record.index = iRow;
    record.multiplier = solution.col_value[iRow] / multiplier_norm;
    ill_conditioning.record.push_back(record);
  }
  HighsSparseMatrix& incumbent_matrix = incumbent_lp.a_matrix_;
  if (constraint) {
    HighsInt num_nz;
    std::vector<HighsInt> index(incumbent_lp.num_col_);
    std::vector<double> value(incumbent_lp.num_col_);
    HighsInt* p_index = index.data();
    double* p_value = value.data();
    for (HighsInt iX = 0; iX < HighsInt(ill_conditioning.record.size()); iX++) {
      ss.str(std::string());
      bool newline = false;
      HighsInt iRow = ill_conditioning.record[iX].index;
      double multiplier = ill_conditioning.record[iX].multiplier;
      // Extract the row corresponding to this constraint
      num_nz = 0;
      incumbent_matrix.getRow(iRow, num_nz, p_index, p_value);
      std::string row_name = has_row_names ? incumbent_lp.row_names_[iRow]
                                           : "R" + std::to_string(iRow);
      ss << "(Mu=" << multiplier << ")" << row_name << ": ";
      const double lower = incumbent_lp.row_lower_[iRow];
      const double upper = incumbent_lp.row_upper_[iRow];
      if (lower > -kHighsInf && lower != upper)
        ss << incumbent_lp.row_lower_[iRow] << " <= ";
      for (HighsInt iEl = 0; iEl < num_nz; iEl++) {
        if (newline) {
          ss << "  ";
          newline = false;
        }
        HighsInt iCol = index[iEl];
        printCoefficient(value[iEl], iEl == 0);
        std::string col_name = has_col_names ? incumbent_lp.col_names_[iCol]
                                             : "C" + std::to_string(iCol);
        ss << col_name << " ";
        HighsInt length_ss = ss.str().length();
        if (length_ss > 72 && iEl < num_nz - 1) {
          highsLogUser(options_.log_options, HighsLogType::kInfo, "%s\n",
                       ss.str().c_str());
          ss.str(std::string());
          newline = true;
        }
      }
      if (upper < kHighsInf) {
        if (lower == upper) {
          ss << "= " << upper;
        } else {
          ss << "<= " << upper;
        }
      }
      if (ss.str().length())
        highsLogUser(options_.log_options, HighsLogType::kInfo, "%s\n",
                     ss.str().c_str());
    }
  } else {
    for (const auto& rec : ill_conditioning.record) {
      ss.str(std::string());
      bool newline = false;
      double multiplier = rec.multiplier;
      HighsInt iCol = basic_var[rec.index];
      if (iCol < incumbent_lp.num_col_) {
        std::string col_name = has_col_names ? incumbent_lp.col_names_[iCol]
                                             : "C" + std::to_string(iCol);
        ss << "(Mu=" << multiplier << ")" << col_name << ": ";
        for (HighsInt iEl = incumbent_matrix.start_[iCol];
             iEl < incumbent_matrix.start_[iCol + 1]; iEl++) {
          if (newline) {
            ss << "  ";
            newline = false;
          } else {
            if (iEl > incumbent_matrix.start_[iCol]) ss << " | ";
          }
          HighsInt iRow = incumbent_matrix.index_[iEl];
          printCoefficient(incumbent_matrix.value_[iEl], true);
          std::string row_name = has_row_names ? incumbent_lp.row_names_[iRow]
                                               : "R" + std::to_string(iRow);
          ss << row_name;
          HighsInt length_ss = ss.str().length();
          if (length_ss > 72 && iEl < incumbent_matrix.start_[iCol + 1] - 1) {
            ss << " | ";
            highsLogUser(options_.log_options, HighsLogType::kInfo, "%s\n",
                         ss.str().c_str());
            ss.str(std::string());
            newline = true;
          }
        }
      } else {
        HighsInt iRow = iCol - incumbent_lp.num_col_;
        std::string col_name = has_row_names
                                   ? "Slack_" + incumbent_lp.row_names_[iRow]
                                   : "Slack_R" + std::to_string(iRow);
        ss << "(Mu=" << multiplier << ")" << col_name << ": ";
      }
      if (ss.str().length())
        highsLogUser(options_.log_options, HighsLogType::kInfo, "%s\n",
                     ss.str().c_str());
    }
  }
  return HighsStatus::kOk;
}

void Highs::formIllConditioningLp0(HighsLp& ill_conditioning_lp,
                                   std::vector<HighsInt>& basic_var,
                                   const bool constraint) {
  HighsLp& incumbent_lp = this->model_.lp_;
  // Conditioning LP minimizes the infeasibilities of
  //
  // [B^T]y = [0]; y free - for constraint view
  // [e^T]    [1]
  //
  // [ B ]y = [0]; y free - for column view
  // [e^T]    [1]
  //
  ill_conditioning_lp.num_row_ = incumbent_lp.num_row_ + 1;
  for (HighsInt iRow = 0; iRow < incumbent_lp.num_row_; iRow++) {
    ill_conditioning_lp.row_lower_.push_back(0);
    ill_conditioning_lp.row_upper_.push_back(0);
  }
  ill_conditioning_lp.row_lower_.push_back(1);
  ill_conditioning_lp.row_upper_.push_back(1);
  HighsSparseMatrix& incumbent_matrix = incumbent_lp.a_matrix_;
  incumbent_matrix.ensureColwise();
  HighsSparseMatrix& ill_conditioning_matrix = ill_conditioning_lp.a_matrix_;
  ill_conditioning_matrix.num_row_ = ill_conditioning_lp.num_row_;
  // Form the basis matrix and
  //
  // * For constraint view, add the column e, and transpose the
  // * resulting matrix
  //
  // * For column view, add a unit entry to each column
  //
  const HighsInt ill_conditioning_lp_e_row = ill_conditioning_lp.num_row_ - 1;
  for (HighsInt iCol = 0; iCol < incumbent_lp.num_col_; iCol++) {
    if (this->basis_.col_status[iCol] != HighsBasisStatus::kBasic) continue;
    // Basic column goes into conditioning LP, possibly with unit
    // coefficient for constraint e^Ty=1
    basic_var.push_back(iCol);
    ill_conditioning_lp.col_cost_.push_back(0);
    ill_conditioning_lp.col_lower_.push_back(-kHighsInf);
    ill_conditioning_lp.col_upper_.push_back(kHighsInf);
    for (HighsInt iEl = incumbent_matrix.start_[iCol];
         iEl < incumbent_matrix.start_[iCol + 1]; iEl++) {
      ill_conditioning_matrix.index_.push_back(incumbent_matrix.index_[iEl]);
      ill_conditioning_matrix.value_.push_back(incumbent_matrix.value_[iEl]);
    }
    if (!constraint) {
      ill_conditioning_matrix.index_.push_back(ill_conditioning_lp_e_row);
      ill_conditioning_matrix.value_.push_back(1.0);
    }
    ill_conditioning_matrix.start_.push_back(
        HighsInt(ill_conditioning_matrix.index_.size()));
  }
  for (HighsInt iRow = 0; iRow < incumbent_lp.num_row_; iRow++) {
    if (this->basis_.row_status[iRow] != HighsBasisStatus::kBasic) continue;
    // Basic slack goes into conditioning LP
    basic_var.push_back(incumbent_lp.num_col_ + iRow);
    ill_conditioning_lp.col_cost_.push_back(0);
    ill_conditioning_lp.col_lower_.push_back(-kHighsInf);
    ill_conditioning_lp.col_upper_.push_back(kHighsInf);
    ill_conditioning_matrix.index_.push_back(iRow);
    ill_conditioning_matrix.value_.push_back(-1.0);
    if (!constraint) {
      ill_conditioning_matrix.index_.push_back(ill_conditioning_lp_e_row);
      ill_conditioning_matrix.value_.push_back(1.0);
    }
    ill_conditioning_matrix.start_.push_back(
        HighsInt(ill_conditioning_matrix.index_.size()));
  }
  if (constraint) {
    // Add the column e, and transpose the resulting matrix
    for (HighsInt iRow = 0; iRow < incumbent_lp.num_row_; iRow++) {
      ill_conditioning_matrix.index_.push_back(iRow);
      ill_conditioning_matrix.value_.push_back(1.0);
    }
    ill_conditioning_matrix.start_.push_back(
        HighsInt(ill_conditioning_matrix.index_.size()));
    ill_conditioning_matrix.num_row_ = incumbent_lp.num_row_;
    ill_conditioning_matrix.num_col_ = incumbent_lp.num_row_ + 1;
    ill_conditioning_matrix.ensureRowwise();
    ill_conditioning_matrix.format_ = MatrixFormat::kColwise;
  }
  // Now add the variables to measure the infeasibilities
  for (HighsInt iRow = 0; iRow < incumbent_lp.num_row_; iRow++) {
    // Adding x_+ with cost 1
    ill_conditioning_lp.col_cost_.push_back(1);
    ill_conditioning_lp.col_lower_.push_back(0);
    ill_conditioning_lp.col_upper_.push_back(kHighsInf);
    ill_conditioning_matrix.index_.push_back(iRow);
    ill_conditioning_matrix.value_.push_back(1.0);
    ill_conditioning_matrix.start_.push_back(
        HighsInt(ill_conditioning_matrix.index_.size()));
    // Subtracting x_- with cost 1
    ill_conditioning_lp.col_cost_.push_back(1);
    ill_conditioning_lp.col_lower_.push_back(0);
    ill_conditioning_lp.col_upper_.push_back(kHighsInf);
    ill_conditioning_matrix.index_.push_back(iRow);
    ill_conditioning_matrix.value_.push_back(-1.0);
    ill_conditioning_matrix.start_.push_back(
        HighsInt(ill_conditioning_matrix.index_.size()));
  }
  ill_conditioning_lp.num_col_ = 3 * incumbent_lp.num_row_;
  ill_conditioning_matrix.num_col_ = ill_conditioning_lp.num_col_;
  ill_conditioning_matrix.num_row_ = ill_conditioning_lp.num_row_;
}

void Highs::formIllConditioningLp1(HighsLp& ill_conditioning_lp,
                                   std::vector<HighsInt>& basic_var,
                                   const bool constraint,
                                   const double ill_conditioning_bound) {
  HighsLp& incumbent_lp = this->model_.lp_;
  const HighsInt incumbent_num_row = incumbent_lp.num_row_;
  //
  // Using notation from Klotz14
  //
  // For constraint view, conditioning LP minimizes the
  // infeasibilities of c7
  //
  // c4: B^Ty         -   s +   t   = 0
  // c1:    y - u + w               = 0
  // c7:        u + w               = 0
  // c6: e^Ty                       = 1
  // c5:               e^Ts + e^Tt <= eps
  // y free; u, w, s, t >= 0
  //
  // Column view uses B rather than B^T
  //
  // Set up offsets
  //
  const HighsInt c4_offset = 0;
  const HighsInt c1_offset = incumbent_num_row;
  const HighsInt c7_offset = 2 * incumbent_num_row;
  const HighsInt c6_offset = 3 * incumbent_num_row;
  const HighsInt c5_offset = 3 * incumbent_num_row + 1;
  for (HighsInt iRow = 0; iRow < c6_offset; iRow++) {
    ill_conditioning_lp.row_lower_.push_back(0);
    ill_conditioning_lp.row_upper_.push_back(0);
  }
  HighsSparseMatrix& incumbent_matrix = incumbent_lp.a_matrix_;
  incumbent_matrix.ensureColwise();
  HighsSparseMatrix& ill_conditioning_matrix = ill_conditioning_lp.a_matrix_;
  // Form the basis matrix and
  //
  // * For constraint view, add the identity matrix and vector of
  // * ones, and transpose the resulting matrix
  //
  // * For column view, add an identity matrix column and unit entry
  // * below each column
  //
  ill_conditioning_lp.num_col_ = 0;
  for (HighsInt iCol = 0; iCol < incumbent_lp.num_col_; iCol++) {
    if (this->basis_.col_status[iCol] != HighsBasisStatus::kBasic) continue;
    // Basic column goes into ill-conditioning LP, possibly with
    // identity matrix column for constraint y - u + w = 0 and unit
    // entry for e^Ty = 1
    basic_var.push_back(iCol);
    ill_conditioning_lp.col_names_.push_back(
        "y_" + std::to_string(ill_conditioning_lp.num_col_));
    ill_conditioning_lp.col_cost_.push_back(0);
    ill_conditioning_lp.col_lower_.push_back(-kHighsInf);
    ill_conditioning_lp.col_upper_.push_back(kHighsInf);
    for (HighsInt iEl = incumbent_matrix.start_[iCol];
         iEl < incumbent_matrix.start_[iCol + 1]; iEl++) {
      ill_conditioning_matrix.index_.push_back(incumbent_matrix.index_[iEl]);
      ill_conditioning_matrix.value_.push_back(incumbent_matrix.value_[iEl]);
    }
    if (!constraint) {
      // Add identity matrix column for constraint y - u + w = 0
      ill_conditioning_matrix.index_.push_back(c1_offset +
                                               ill_conditioning_lp.num_col_);
      ill_conditioning_matrix.value_.push_back(1.0);
      // Add unit entry for e^Ty = 1
      ill_conditioning_matrix.index_.push_back(c6_offset);
      ill_conditioning_matrix.value_.push_back(1.0);
    }
    ill_conditioning_matrix.start_.push_back(
        HighsInt(ill_conditioning_matrix.index_.size()));
    ill_conditioning_lp.num_col_++;
  }

  for (HighsInt iRow = 0; iRow < incumbent_num_row; iRow++) {
    if (this->basis_.row_status[iRow] != HighsBasisStatus::kBasic) continue;
    // Basic slack goes into conditioning LP
    basic_var.push_back(incumbent_lp.num_col_ + iRow);
    ill_conditioning_lp.col_names_.push_back(
        "y_" + std::to_string(ill_conditioning_lp.num_col_));
    ill_conditioning_lp.col_cost_.push_back(0);
    ill_conditioning_lp.col_lower_.push_back(-kHighsInf);
    ill_conditioning_lp.col_upper_.push_back(kHighsInf);
    ill_conditioning_matrix.index_.push_back(iRow);
    ill_conditioning_matrix.value_.push_back(-1.0);
    if (!constraint) {
      // Add identity matrix column for constraint y - u + w = 0
      ill_conditioning_matrix.index_.push_back(c1_offset +
                                               ill_conditioning_lp.num_col_);
      ill_conditioning_matrix.value_.push_back(1.0);
      // Add unit entry for e^Ty = 1
      ill_conditioning_matrix.index_.push_back(c6_offset);
      ill_conditioning_matrix.value_.push_back(1.0);
    }
    ill_conditioning_matrix.start_.push_back(
        HighsInt(ill_conditioning_matrix.index_.size()));
    ill_conditioning_lp.num_col_++;
  }
  assert(ill_conditioning_lp.num_col_ == incumbent_num_row);
  if (constraint) {
    // Add the identity matrix for constraint y - u + w = 0
    for (HighsInt iRow = 0; iRow < incumbent_num_row; iRow++) {
      ill_conditioning_matrix.index_.push_back(iRow);
      ill_conditioning_matrix.value_.push_back(1.0);
      ill_conditioning_matrix.start_.push_back(
          HighsInt(ill_conditioning_matrix.index_.size()));
    }
    // Add the square zero matrix of c7
    for (HighsInt iRow = 0; iRow < incumbent_num_row; iRow++)
      ill_conditioning_matrix.start_.push_back(
          HighsInt(ill_conditioning_matrix.index_.size()));
    // Add the vector of ones for e^Ty = 1
    for (HighsInt iRow = 0; iRow < incumbent_num_row; iRow++) {
      ill_conditioning_matrix.index_.push_back(iRow);
      ill_conditioning_matrix.value_.push_back(1.0);
    }
    ill_conditioning_matrix.start_.push_back(
        HighsInt(ill_conditioning_matrix.index_.size()));

    // Transpose the resulting matrix
    ill_conditioning_matrix.num_col_ = c6_offset + 1;
    ill_conditioning_matrix.num_row_ = incumbent_num_row;
    ill_conditioning_matrix.ensureRowwise();
    ill_conditioning_matrix.format_ = MatrixFormat::kColwise;
    ill_conditioning_matrix.num_col_ = incumbent_num_row;
    ill_conditioning_matrix.num_row_ = c6_offset + 1;
  }

  assert(ill_conditioning_lp.num_col_ == incumbent_num_row);
  ill_conditioning_lp.num_row_ = 3 * incumbent_num_row + 2;

  // Now add the variables u and w
  for (HighsInt iRow = 0; iRow < incumbent_num_row; iRow++) {
    // Adding u with cost 0
    ill_conditioning_lp.col_names_.push_back("u_" + std::to_string(iRow));
    ill_conditioning_lp.col_cost_.push_back(0);
    ill_conditioning_lp.col_lower_.push_back(0);
    ill_conditioning_lp.col_upper_.push_back(kHighsInf);
    // Contribution to c1: y - u + w = 0
    ill_conditioning_matrix.index_.push_back(c1_offset + iRow);
    ill_conditioning_matrix.value_.push_back(-1.0);
    // Contribution to c7: u + w = 0
    ill_conditioning_matrix.index_.push_back(c7_offset + iRow);
    ill_conditioning_matrix.value_.push_back(1.0);
    ill_conditioning_matrix.start_.push_back(
        HighsInt(ill_conditioning_matrix.index_.size()));
    ill_conditioning_lp.num_col_++;
    // Adding w with cost 0
    ill_conditioning_lp.col_names_.push_back("w_" + std::to_string(iRow));
    ill_conditioning_lp.col_cost_.push_back(0);
    ill_conditioning_lp.col_lower_.push_back(0);
    ill_conditioning_lp.col_upper_.push_back(kHighsInf);
    // Contribution to c1: y - u + w = 0
    ill_conditioning_matrix.index_.push_back(c1_offset + iRow);
    ill_conditioning_matrix.value_.push_back(1.0);
    // Contribution to c7: u + w = 0
    ill_conditioning_matrix.index_.push_back(c7_offset + iRow);
    ill_conditioning_matrix.value_.push_back(1.0);
    ill_conditioning_matrix.start_.push_back(
        HighsInt(ill_conditioning_matrix.index_.size()));
    ill_conditioning_lp.num_col_++;
  }
  // Now add the variables s and t
  for (HighsInt iRow = 0; iRow < incumbent_num_row; iRow++) {
    // Adding s with cost 0
    ill_conditioning_lp.col_names_.push_back("s_" + std::to_string(iRow));
    ill_conditioning_lp.col_cost_.push_back(0);
    ill_conditioning_lp.col_lower_.push_back(0);
    ill_conditioning_lp.col_upper_.push_back(kHighsInf);
    // Contribution to c4: B^Ty - s + t = 0
    ill_conditioning_matrix.index_.push_back(c4_offset + iRow);
    ill_conditioning_matrix.value_.push_back(-1.0);
    // Contribution to c5: e^Ts + e^Tt <= eps
    ill_conditioning_matrix.index_.push_back(c5_offset);
    ill_conditioning_matrix.value_.push_back(1.0);
    ill_conditioning_matrix.start_.push_back(
        HighsInt(ill_conditioning_matrix.index_.size()));
    ill_conditioning_lp.num_col_++;
    // Adding t with cost 0
    ill_conditioning_lp.col_names_.push_back("t_" + std::to_string(iRow));
    ill_conditioning_lp.col_cost_.push_back(0);
    ill_conditioning_lp.col_lower_.push_back(0);
    ill_conditioning_lp.col_upper_.push_back(kHighsInf);
    // Contribution to c4: B^Ty - s + t = 0
    ill_conditioning_matrix.index_.push_back(c4_offset + iRow);
    ill_conditioning_matrix.value_.push_back(1.0);
    // Contribution to c5: e^Ts + e^Tt <= eps
    ill_conditioning_matrix.index_.push_back(c5_offset);
    ill_conditioning_matrix.value_.push_back(1.0);
    ill_conditioning_matrix.start_.push_back(
        HighsInt(ill_conditioning_matrix.index_.size()));
    ill_conditioning_lp.num_col_++;
  }
  // Add the bounds for c6: e^Ty = 1
  ill_conditioning_lp.row_lower_.push_back(1);
  ill_conditioning_lp.row_upper_.push_back(1);
  // Add the bounds for c5: e^Ts + e^Tt <= eps
  assert(ill_conditioning_bound > 0);
  ill_conditioning_lp.row_lower_.push_back(-kHighsInf);
  ill_conditioning_lp.row_upper_.push_back(ill_conditioning_bound);
  assert(HighsInt(ill_conditioning_lp.row_lower_.size()) ==
         ill_conditioning_lp.num_row_);
  assert(HighsInt(ill_conditioning_lp.row_upper_.size()) ==
         ill_conditioning_lp.num_row_);

  // Now add the variables to measure the infeasibilities in
  //
  // c7: u + w = r^+ - r^-
  for (HighsInt iRow = 0; iRow < incumbent_num_row; iRow++) {
    // Adding r^+ with cost 1
    ill_conditioning_lp.col_names_.push_back("IfsPlus_" + std::to_string(iRow));
    ill_conditioning_lp.col_cost_.push_back(1);
    ill_conditioning_lp.col_lower_.push_back(0);
    ill_conditioning_lp.col_upper_.push_back(kHighsInf);
    ill_conditioning_matrix.index_.push_back(c7_offset + iRow);
    ill_conditioning_matrix.value_.push_back(-1.0);
    ill_conditioning_matrix.start_.push_back(
        HighsInt(ill_conditioning_matrix.index_.size()));
    ill_conditioning_lp.num_col_++;
    // Adding r^- with cost 1
    ill_conditioning_lp.col_names_.push_back("IfsMinus_" +
                                             std::to_string(iRow));
    ill_conditioning_lp.col_cost_.push_back(1);
    ill_conditioning_lp.col_lower_.push_back(0);
    ill_conditioning_lp.col_upper_.push_back(kHighsInf);
    ill_conditioning_matrix.index_.push_back(c7_offset + iRow);
    ill_conditioning_matrix.value_.push_back(1.0);
    ill_conditioning_matrix.start_.push_back(
        HighsInt(ill_conditioning_matrix.index_.size()));
    ill_conditioning_lp.num_col_++;
  }
  assert(ill_conditioning_lp.num_col_ == 7 * incumbent_num_row);
  assert(ill_conditioning_lp.num_row_ == 3 * incumbent_num_row + 2);
  ill_conditioning_matrix.num_col_ = ill_conditioning_lp.num_col_;
  ill_conditioning_matrix.num_row_ = ill_conditioning_lp.num_row_;
}

bool Highs::infeasibleBoundsOk() {
  const HighsLogOptions& log_options = this->options_.log_options;
  HighsLp& lp = this->model_.lp_;

  HighsInt num_true_infeasible_bound = 0;
  HighsInt num_ok_infeasible_bound = 0;
  const bool has_integrality = lp.integrality_.size() > 0;
  bool performed_inward_integer_rounding = false;
  // Lambda for assessing infeasible bounds
  auto infeasibleBoundOk = [&](const std::string type, const HighsInt iX,
                               double& lower, double& upper) {
    double range = upper - lower;
    // Should only be called if lower > upper, so range < 0
    assert(range < 0);
    if (range >= 0) return true;
    if (range > -this->options_.primal_feasibility_tolerance) {
      // Infeasibility is less than feasibility tolerance, so fix
      // bounds at lower (upper) if lower (upper) is an integer - and
      // both can't be integer, otherwise the range <= -1 - otherwise
      // fix at 0.5 * (lower + upper)
      num_ok_infeasible_bound++;
      bool report = num_ok_infeasible_bound <= 10;
      bool integer_lower = lower == std::floor(lower + 0.5);
      bool integer_upper = upper == std::floor(upper + 0.5);
      assert(!integer_lower || !integer_upper);
      if (integer_lower) {
        if (report)
          highsLogUser(log_options, HighsLogType::kInfo,
                       "%s %d bounds [%g, %g] have infeasibility = %g so set "
                       "upper bound to %g\n",
                       type.c_str(), int(iX), lower, upper, range, lower);
        upper = lower;
      } else if (integer_upper) {
        if (report)
          highsLogUser(log_options, HighsLogType::kInfo,
                       "%s %d bounds [%g, %g] have infeasibility = %g so set "
                       "lower bound to %g\n",
                       type.c_str(), int(iX), lower, upper, range, upper);
        lower = upper;
      } else {
        double mid = 0.5 * (lower + upper);
        if (report)
          highsLogUser(log_options, HighsLogType::kInfo,
                       "%s %d bounds [%g, %g] have infeasibility = %g so set "
                       "both bounds to %g\n",
                       type.c_str(), int(iX), lower, upper, range, mid);
        lower = mid;
        upper = mid;
      }
      return true;
    }
    // Infeasibility is greater than feasibility tolerance, so report
    // this (up to 10 times)
    num_true_infeasible_bound++;
    if (num_true_infeasible_bound <= 10)
      highsLogUser(
          log_options, HighsLogType::kInfo,
          "%s %d bounds [%g, %g] have excessive infeasibility = %g%s\n",
          type.c_str(), int(iX), lower, upper, range,
          performed_inward_integer_rounding ? " due to inward integer rounding"
                                            : "");
    return false;
  };

  const bool perform_inward_integer_rounding = !this->options_.solve_relaxation;
  for (HighsInt iCol = 0; iCol < lp.num_col_; iCol++) {
    performed_inward_integer_rounding = false;
    double lower = lp.col_lower_[iCol];
    double upper = lp.col_upper_[iCol];
    if (has_integrality) {
      // Semi-variables cannot have inconsistent bounds
      if (lp.integrality_[iCol] == HighsVarType::kSemiContinuous ||
          lp.integrality_[iCol] == HighsVarType::kSemiInteger)
        continue;
      if (perform_inward_integer_rounding &&
          lp.integrality_[iCol] == HighsVarType::kInteger) {
        // Assess bounds after inward integer rounding
        double feastol = this->options_.mip_feasibility_tolerance;
        double integer_lower = std::ceil(lower - feastol);
        double integer_upper = std::floor(upper + feastol);
        assert(integer_lower >= lower);
        assert(integer_upper <= upper);
        performed_inward_integer_rounding =
            integer_lower > lower || integer_upper < upper;
        lower = integer_lower;
        upper = integer_upper;
      }
    }
    //
    if (lower > upper) {
      if (infeasibleBoundOk("Column", iCol, lower, upper)) {
        // Bound infeasibility is OK (less than the tolerance), so can
        // change the model data
        lp.col_lower_[iCol] = lower;
        lp.col_upper_[iCol] = upper;
      }
    }
    // Note that any inward integer rounding can't be used to change
    // the model data, since it may be a significant change and make
    // the relaxation infeasible when previously it was feasible. In
    // particular, when inward integer rounding leads to inconsistent
    // bounds being propagated to the relaxation, this can prevent a
    // dual ray from being constructed
  }
  performed_inward_integer_rounding = false;
  for (HighsInt iRow = 0; iRow < lp.num_row_; iRow++) {
    if (lp.row_lower_[iRow] > lp.row_upper_[iRow])
      infeasibleBoundOk("Row", iRow, lp.row_lower_[iRow], lp.row_upper_[iRow]);
  }
  if (num_ok_infeasible_bound > 0)
    highsLogUser(log_options, HighsLogType::kInfo,
                 "Model has %d small inconsistent bound(s): rectified\n",
                 int(num_ok_infeasible_bound));
  if (num_true_infeasible_bound > 0)
    highsLogUser(log_options, HighsLogType::kInfo,
                 "Model has %d significant inconsistent bound(s): infeasible\n",
                 int(num_true_infeasible_bound));
  return num_true_infeasible_bound == 0;
}

bool Highs::validLinearObjective(const HighsLinearObjective& linear_objective,
                                 const HighsInt iObj) const {
  HighsInt linear_objective_coefficients_size =
      linear_objective.coefficients.size();
  if (linear_objective_coefficients_size != this->model_.lp_.num_col_) {
    highsLogUser(
        options_.log_options, HighsLogType::kError,
        "Coefficient vector for linear objective %s has size %d != %d = "
        "lp.num_col_\n",
        iObj >= 0 ? std::to_string(iObj).c_str() : "",
        int(linear_objective_coefficients_size),
        int(this->model_.lp_.num_col_));
    return false;
  }
  if (!options_.blend_multi_objectives &&
      hasRepeatedLinearObjectivePriorities(&linear_objective)) {
    highsLogUser(
        options_.log_options, HighsLogType::kError,
        "Repeated priorities for lexicographic optimization is illegal\n");
    return false;
  }
  return true;
}

bool Highs::hasRepeatedLinearObjectivePriorities(
    const HighsLinearObjective* linear_objective) const {
  // Look for repeated values in the linear objective priorities, also
  // comparing linear_objective if it's not a null pointer. Cost is
  // O(n^2), but who will have more than O(1) linear objectives!
  HighsInt num_linear_objective = this->multi_linear_objective_.size();
  if (num_linear_objective <= 0 ||
      (num_linear_objective <= 1 && !linear_objective))
    return false;
  for (HighsInt iObj0 = 0; iObj0 < num_linear_objective; iObj0++) {
    HighsInt priority0 = this->multi_linear_objective_[iObj0].priority;
    for (HighsInt iObj1 = iObj0 + 1; iObj1 < num_linear_objective; iObj1++) {
      HighsInt priority1 = this->multi_linear_objective_[iObj1].priority;
      if (priority1 == priority0) return true;
    }
    if (linear_objective) {
      if (linear_objective->priority == priority0) return true;
    }
  }
  return false;
}

static bool comparison(std::pair<HighsInt, HighsInt> x1,
                       std::pair<HighsInt, HighsInt> x2) {
  return x1.first >= x2.first;
}

HighsStatus Highs::returnFromLexicographicOptimization(
    HighsStatus return_status, HighsInt original_lp_num_row) {
  // Save model_status_ and info_ since they are cleared by calling
  // deleteRows
  HighsModelStatus model_status = this->model_status_;
  HighsInfo info = this->info_;
  HighsInt num_linear_objective = this->multi_linear_objective_.size();
  if (num_linear_objective > 1) {
    this->deleteRows(original_lp_num_row, this->model_.lp_.num_row_ - 1);
    // Recover model_status_ and info_, and then account for lack of basis or
    // dual solution
    this->model_status_ = model_status;
    this->info_ = info;
    info_.objective_function_value = 0;
    info_.basis_validity = kBasisValidityInvalid;
    info_.invalidateDualKkt();
    this->solution_.value_valid = true;
    this->model_.lp_.col_cost_.assign(this->model_.lp_.num_col_, 0);
  }
  return return_status;
}

HighsStatus Highs::multiobjectiveSolve() {
  const HighsInt coeff_logging_size_limit = 10;
  HighsInt num_linear_objective = this->multi_linear_objective_.size();

  assert(num_linear_objective > 0);
  HighsLp& lp = this->model_.lp_;
  for (HighsInt iObj = 0; iObj < num_linear_objective; iObj++) {
    HighsLinearObjective& multi_linear_objective =
        this->multi_linear_objective_[iObj];
    if (multi_linear_objective.coefficients.size() !=
        static_cast<size_t>(lp.num_col_)) {
      highsLogUser(options_.log_options, HighsLogType::kError,
                   "Multiple linear objective coefficient vector %d has size "
                   "incompatible with model\n",
                   int(iObj));
      return HighsStatus::kError;
    }
  }

  std::unique_ptr<std::stringstream> multi_objective_log;
  highsLogUser(options_.log_options, HighsLogType::kInfo,
               "Solving with %d multiple linear objectives, %s\n",
               int(num_linear_objective),
               this->options_.blend_multi_objectives
                   ? "blending objectives by weight"
                   : "using lexicographic optimization by priority");
  highsLogUser(
      options_.log_options, HighsLogType::kInfo,
      "Ix      weight      offset     abs_tol     rel_tol    priority%s\n",
      lp.num_col_ < coeff_logging_size_limit ? "   coefficients" : "");
  for (HighsInt iObj = 0; iObj < num_linear_objective; iObj++) {
    HighsLinearObjective& linear_objective =
        this->multi_linear_objective_[iObj];
    multi_objective_log =
        std::unique_ptr<std::stringstream>(new std::stringstream());
    *multi_objective_log << highsFormatToString(
        "%2d %11.6g %11.6g %11.6g %11.6g %11d  ", int(iObj),
        linear_objective.weight, linear_objective.offset,
        linear_objective.abs_tolerance, linear_objective.rel_tolerance,
        int(linear_objective.priority));
    if (lp.num_col_ < coeff_logging_size_limit) {
      for (HighsInt iCol = 0; iCol < lp.num_col_; iCol++)
        *multi_objective_log << highsFormatToString(
            "%s c_{%1d} = %g", iCol == 0 ? "" : ",", int(iCol),
            linear_objective.coefficients[iCol]);
    }
    *multi_objective_log << "\n";
    highsLogUser(options_.log_options, HighsLogType::kInfo, "%s",
                 multi_objective_log->str().c_str());
  }
  // Solving with a different objective, but don't call
  // this->clearSolver() since this loses the current solution - that
  // may have been provided by the user (#2419). Just clear the dual
  // data.
  //
  this->clearSolverDualData();
  if (this->options_.blend_multi_objectives) {
    // Objectives are blended by weight and minimized
    lp.offset_ = 0;
    lp.col_cost_.assign(lp.num_col_, 0);
    for (HighsInt iObj = 0; iObj < num_linear_objective; iObj++) {
      HighsLinearObjective& multi_linear_objective =
          this->multi_linear_objective_[iObj];
      lp.offset_ +=
          multi_linear_objective.weight * multi_linear_objective.offset;
      for (HighsInt iCol = 0; iCol < lp.num_col_; iCol++)
        lp.col_cost_[iCol] += multi_linear_objective.weight *
                              multi_linear_objective.coefficients[iCol];
    }
    lp.sense_ = ObjSense::kMinimize;

    multi_objective_log =
        std::unique_ptr<std::stringstream>(new std::stringstream());
    *multi_objective_log << highsFormatToString(
        "Solving with blended objective");
    if (lp.num_col_ < coeff_logging_size_limit) {
      *multi_objective_log << highsFormatToString(
          ": %s %g", lp.sense_ == ObjSense::kMinimize ? "min" : "max",
          lp.offset_);
      for (HighsInt iCol = 0; iCol < lp.num_col_; iCol++)
        *multi_objective_log << highsFormatToString(
            " + (%g) x[%d]", lp.col_cost_[iCol], int(iCol));
    }
    *multi_objective_log << "\n";
    highsLogUser(options_.log_options, HighsLogType::kInfo, "%s",
                 multi_objective_log->str().c_str());
    return this->optimizeModel();
  }

  // Objectives are applied lexicographically
  if (model_.isQp() && num_linear_objective > 1) {
    // Lexicographic optimization with a single linear objective is
    // trivially standard optimization, so is OK
    highsLogUser(
        options_.log_options, HighsLogType::kError,
        "Cannot perform non-trivial lexicographic optimization for QP\n");
    return HighsStatus::kError;
  }
  // Check whether there are repeated linear objective priorities
  if (hasRepeatedLinearObjectivePriorities()) {
    highsLogUser(
        options_.log_options, HighsLogType::kError,
        "Repeated priorities for lexicographic optimization is illegal\n");
    return HighsStatus::kError;
  }
  std::vector<std::pair<HighsInt, HighsInt>> priority_objective;

  for (HighsInt iObj = 0; iObj < num_linear_objective; iObj++)
    priority_objective.push_back(
        std::make_pair(this->multi_linear_objective_[iObj].priority, iObj));
  std::sort(priority_objective.begin(), priority_objective.end(), comparison);
  // Clear LP objective
  lp.offset_ = 0;
  lp.col_cost_.assign(lp.num_col_, 0);
  const HighsInt original_lp_num_row = lp.num_row_;
  std::vector<HighsInt> index(lp.num_col_);
  std::vector<double> value(lp.num_col_);
  // Use the solution of one MIP to provide an integer feasible
  // solution of the next
  HighsSolution solution;
  for (HighsInt iIx = 0; iIx < num_linear_objective; iIx++) {
    HighsInt priority = priority_objective[iIx].first;
    HighsInt iObj = priority_objective[iIx].second;
    // Use this objective
    HighsLinearObjective& linear_objective =
        this->multi_linear_objective_[iObj];
    lp.offset_ = linear_objective.offset;
    lp.col_cost_ = linear_objective.coefficients;
    lp.sense_ =
        linear_objective.weight > 0 ? ObjSense::kMinimize : ObjSense::kMaximize;
    if (lp.isMip() && solution.value_valid) {
      HighsStatus set_solution_status = this->setSolution(solution);
      if (set_solution_status == HighsStatus::kError) {
        highsLogUser(options_.log_options, HighsLogType::kError,
                     "Failure to use one MIP to provide an integer feasible "
                     "solution of the next\n");
        return returnFromLexicographicOptimization(HighsStatus::kError,
                                                   original_lp_num_row);
      }
      bool valid, integral, feasible;
      HighsStatus assess_primal_solution =
          assessPrimalSolution(valid, integral, feasible);
      if (!valid || !integral || !feasible) {
        highsLogUser(options_.log_options, HighsLogType::kWarning,
                     "Failure to use one MIP to provide an integer feasible "
                     "solution of the next: "
                     "status is valid = %s, integral = %s, feasible = %s\n",
                     highsBoolToString(valid).c_str(),
                     highsBoolToString(integral).c_str(),
                     highsBoolToString(feasible).c_str());
      }
    }
    multi_objective_log =
        std::unique_ptr<std::stringstream>(new std::stringstream());
    *multi_objective_log << highsFormatToString("Solving with objective %d",
                                                int(iObj));
    if (lp.num_col_ < coeff_logging_size_limit) {
      *multi_objective_log << highsFormatToString(
          ": %s %g", lp.sense_ == ObjSense::kMinimize ? "min" : "max",
          lp.offset_);
      for (HighsInt iCol = 0; iCol < lp.num_col_; iCol++)
        *multi_objective_log << highsFormatToString(
            " + (%g) x[%d]", lp.col_cost_[iCol], int(iCol));
    }
    *multi_objective_log << "\n";
    highsLogUser(options_.log_options, HighsLogType::kInfo, "%s",
                 multi_objective_log->str().c_str());
    HighsStatus optimize_model_status = this->optimizeModel();
    if (optimize_model_status == HighsStatus::kError)
      return returnFromLexicographicOptimization(HighsStatus::kError,
                                                 original_lp_num_row);
    if (model_status_ != HighsModelStatus::kOptimal) {
      highsLogUser(options_.log_options, HighsLogType::kWarning,
                   "After priority %d solve, model status is %s\n",
                   int(priority), modelStatusToString(model_status_).c_str());
      return returnFromLexicographicOptimization(HighsStatus::kWarning,
                                                 original_lp_num_row);
    }
    if (iIx == num_linear_objective - 1) break;
    if (lp.isMip()) {
      // Save the solution to provide an integer feasible solution of
      // the next MIP
      solution.col_value = this->solution_.col_value;
      solution.value_valid = true;
    }
    // Add the constraint
    HighsInt nnz = 0;
    for (HighsInt iCol = 0; iCol < lp.num_col_; iCol++) {
      if (lp.col_cost_[iCol]) {
        index[nnz] = iCol;
        value[nnz] = lp.col_cost_[iCol];
        nnz++;
      }
    }
    double objective = info_.objective_function_value;
    HighsStatus add_row_status;
    double lower_bound = -kHighsInf;
    double upper_bound = kHighsInf;
    if (lp.sense_ == ObjSense::kMinimize) {
      // Minimizing, so set a greater upper bound than the objective
      if (linear_objective.abs_tolerance >= 0)
        upper_bound = objective + linear_objective.abs_tolerance;
      if (linear_objective.rel_tolerance >= 0) {
        if (objective >= 0) {
          // Guarantees objective of at least (1+t).f^*
          //
          // so ((1+t).f^*-f^*)/f^* = t
          upper_bound = std::min(
              objective * (1.0 + linear_objective.rel_tolerance), upper_bound);
        } else if (objective < 0) {
          // Guarantees objective of at least (1-t).f^*
          //
          // so ((1-t).f^*-f^*)/f^* = -t
          upper_bound = std::min(
              objective * (1.0 - linear_objective.rel_tolerance), upper_bound);
        }
      }
      upper_bound -= lp.offset_;
    } else {
      // Maximizing, so set a lesser lower bound than the objective
      if (linear_objective.abs_tolerance >= 0)
        lower_bound = objective - linear_objective.abs_tolerance;
      if (linear_objective.rel_tolerance >= 0) {
        if (objective >= 0) {
          // Guarantees objective of at most (1-t).f^*
          //
          // so ((1-t).f^*-f^*)/f^* = -t
          lower_bound = std::max(
              objective * (1.0 - linear_objective.rel_tolerance), lower_bound);
        } else if (objective < 0) {
          // Guarantees objective of at least (1+t).f^*
          //
          // so ((1+t).f^*-f^*)/f^* = t
          lower_bound = std::max(
              objective * (1.0 + linear_objective.rel_tolerance), lower_bound);
        }
      }
      lower_bound -= lp.offset_;
    }
    if (lower_bound == -kHighsInf && upper_bound == kHighsInf)
      highsLogUser(options_.log_options, HighsLogType::kWarning,
                   "After priority %d solve, no objective constraint due to "
                   "absolute tolerance being %g < 0,"
                   " and relative tolerance being %g < 0\n",
                   int(priority), linear_objective.abs_tolerance,
                   linear_objective.rel_tolerance);
    multi_objective_log =
        std::unique_ptr<std::stringstream>(new std::stringstream());
    *multi_objective_log << highsFormatToString(
        "Add constraint for objective %d: ", int(iObj));
    if (nnz < coeff_logging_size_limit) {
      *multi_objective_log << highsFormatToString("%g <= ", lower_bound);
      for (HighsInt iEl = 0; iEl < nnz; iEl++)
        *multi_objective_log << highsFormatToString(
            "%s(%g) x[%d]", iEl > 0 ? " + " : "", value[iEl], int(index[iEl]));
      *multi_objective_log << highsFormatToString(" <= %g\n", upper_bound);
    } else {
      *multi_objective_log << highsFormatToString("Bounds [%g, %g]\n",
                                                  lower_bound, upper_bound);
    }
    highsLogUser(options_.log_options, HighsLogType::kInfo, "%s",
                 multi_objective_log->str().c_str());
    add_row_status =
        this->addRow(lower_bound, upper_bound, nnz, index.data(), value.data());
    assert(add_row_status == HighsStatus::kOk);
  }
  return returnFromLexicographicOptimization(HighsStatus::kOk,
                                             original_lp_num_row);
}

bool Highs::tryPdlpCleanup(HighsInt& pdlp_cleanup_iteration_limit,
                           const HighsInfo& presolved_lp_info) const {
  // Primal/dual infeasibilities/residuals can be magnified in
  // postsolve after PDLP, and IPX without crossover can fail,
  // both leading to model_status_ == HighsModelStatus::kUnknown.
  //
  // If the primal/dual infeasibilities/residuals are too large, then it's not
  // worth it, so measure this
  //
  const double tolerance_margin = 1e2;
  bool no_cleanup = false;
  double max_relative_violation = 0;
  // Lambda for updating no_cleanup and max_relative_violation
  auto noCleanup = [&](const std::string& kkt_name, const double kkt_error,
                       const double kkt_tolerance) {
    double use_kkt_tolerance =
        this->options_.kkt_tolerance != kDefaultKktTolerance
            ? this->options_.kkt_tolerance
            : kkt_tolerance;
    double relative_violation = kkt_error / use_kkt_tolerance;
    if (relative_violation > tolerance_margin)
      printf(
          "KKT measure (%11.4g, %11.4g) gives relative violation of %11.4g for "
          "%s\n",
          kkt_error, use_kkt_tolerance, relative_violation, kkt_name.c_str());
    max_relative_violation =
        std::max(relative_violation, max_relative_violation);
    no_cleanup = max_relative_violation > tolerance_margin;
  };
  noCleanup("Max relative primal infeasibility",
            this->info_.max_relative_primal_infeasibility,
            this->options_.primal_feasibility_tolerance);
  noCleanup("Max relative dual infeasibility",
            this->info_.max_relative_dual_infeasibility,
            this->options_.dual_feasibility_tolerance);
  noCleanup("Max relative primal residual error",
            this->info_.max_relative_primal_residual_error,
            this->options_.primal_residual_tolerance);
  noCleanup("Max relative dual residual error",
            this->info_.max_relative_dual_residual_error,
            this->options_.dual_residual_tolerance);
  noCleanup("Primal-dual objective error",
            this->info_.primal_dual_objective_error,
            this->options_.optimality_tolerance);
  if (no_cleanup) {
    highsLogUser(options_.log_options, HighsLogType::kInfo,
                 "No PDLP cleanup due to KKT errors exceeding tolerances by a "
                 "max factor = %g > %g = allowed margin\n",
                 max_relative_violation, tolerance_margin);
    return false;
  }
  //
  // Force PDLP to be used with an iteration limit
  if (presolved_lp_info.pdlp_iteration_count > 0) {
    // PDLP was used, so allow 10% of the iterations to clean up
    HighsInt ten_percent_pdlp_iteration_count =
        presolved_lp_info.pdlp_iteration_count / 10;
    pdlp_cleanup_iteration_limit =
        std::max(HighsInt(10000), ten_percent_pdlp_iteration_count);
  } else {
    // IPX without crossover was used, so can only guess what PDLP iteration
    // limit to use
    pdlp_cleanup_iteration_limit = 1000;
  }
  return true;
}

void HighsLinearObjective::clear() {
  this->weight = 0.0;
  this->offset = 0.0;
  this->coefficients.clear();
  this->abs_tolerance = 0.0;
  this->rel_tolerance = 0.0;
  this->priority = 0;
}
