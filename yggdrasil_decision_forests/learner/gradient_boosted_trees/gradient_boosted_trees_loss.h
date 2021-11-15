/*
 * Copyright 2021 Google LLC.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Losses for the GBT algorithm.
//
// Losses are implemented by extending the "AbstractLoss" class, and registering
// it the "CreateLoss" function.
//
#ifndef YGGDRASIL_DECISION_FORESTS_LEARNER_GRADIENT_BOOSTED_TREES_LOSS_H_
#define YGGDRASIL_DECISION_FORESTS_LEARNER_GRADIENT_BOOSTED_TREES_LOSS_H_

#include <memory>
#include <random>
#include <string>
#include <vector>

#include "absl/container/inlined_vector.h"
#include "yggdrasil_decision_forests/dataset/data_spec.pb.h"
#include "yggdrasil_decision_forests/dataset/vertical_dataset.h"
#include "yggdrasil_decision_forests/learner/abstract_learner.pb.h"
#include "yggdrasil_decision_forests/learner/decision_tree/training.h"
#include "yggdrasil_decision_forests/learner/gradient_boosted_trees/gradient_boosted_trees.pb.h"
#include "yggdrasil_decision_forests/metric/metric.h"
#include "yggdrasil_decision_forests/metric/ranking_utils.h"
#include "yggdrasil_decision_forests/model/abstract_model.pb.h"
#include "yggdrasil_decision_forests/model/decision_tree/decision_tree.h"
#include "yggdrasil_decision_forests/model/gradient_boosted_trees/gradient_boosted_trees.pb.h"

namespace yggdrasil_decision_forests {
namespace model {
namespace gradient_boosted_trees {

// One dimension of gradients values.
// Also contains the hessian values (is hessian are materialized).
struct GradientData {
  // Values of the gradient. "values[i]" is the gradient of the i-th example.
  // The data is NOT owned by this pointer. In practice, this field is only
  // initialized by "CreateGradientDataset" and points to the data owned by the
  // "sub_train_dataset" VerticalDataset.
  std::vector<float>& gradient;

  // Name of the column containing the gradient data in the virtual training
  // dataset. The virtual training dataset is a shallow copy of the training
  // dataset, with extra columns for the gradients.
  std::string gradient_column_name;

  // Training configuration for the learning of gradient.
  model::proto::TrainingConfig config;
  model::proto::TrainingConfigLinking config_link;

  // Second order derivative of the loss according to the prediction.
  // Only used for Ranking. In order cases (e.g. classification) the second
  // order derivative can be recovered from the gradient and/or is constant, and
  // "second_order_derivative" is empty.
  //
  // The second order derivative to set the leaf values using a newtonian step.
  // This field is only used (currently only for ranking) when the computation
  // of the second order derivative is non trivial (e.g. constant).
  std::vector<float>* hessian = nullptr;

  // Index of the hessian column in the gradient dataset.
  int hessian_col_idx = -1;
};

struct UnitGradientDataRef {
  // Gradient and hessian for each example.
  std::vector<float>* gradient = nullptr;
  std::vector<float>* hessian = nullptr;
};

// Gradient/hessian for each output dimension e.g. n for n-classes
// classification.
typedef absl::InlinedVector<UnitGradientDataRef, 2> GradientDataRef;

// Index of example groups optimized for query. Used for ranking.
class RankingGroupsIndices {
 public:
  // An "Item" is the unit object that is being ranked. For example, a document
  // is an item in a query/document ranking problem.
  struct Item {
    // Ground truth relevance.
    float relevance;
    // Index of the example.
    dataset::VerticalDataset::row_t example_idx;
  };

  // A "group" of examples is a set of examples that share the same "group
  // value" e.g. the same query in a query/document ranking problem.
  struct Group {
    // Value of the group column.
    uint64_t group_idx;
    // Items in the group. Sorted in decreasing order of relevance.
    std::vector<Item> items;
  };

  // Constructs the index. No other function should be called before
  // "Initialize".
  void Initialize(const dataset::VerticalDataset& dataset, int label_col_idx,
                  int group_col_idx);

  double NDCG(const std::vector<float>& predictions,
              const std::vector<float>& weights, int truncation) const;

  const std::vector<Group>& groups() const { return groups_; }

 private:
  static void ExtractPredAndLabelRelevance(
      const std::vector<Item>& group, const std::vector<float>& predictions,
      std::vector<metric::RankingLabelAndPrediction>* pred_and_label_relevance);

  // "groups[i]" is the list of relevance+example_idx of examples with group
  // column equal to "i". "Items" are sorted in decreasing order of relevance.
  std::vector<Group> groups_;

  // Total number of items.
  dataset::VerticalDataset::row_t num_items_ = 0;
};

// Shapes of the loss's outputs.
struct LossShape {
  // Number of dimensions of the gradient.
  int gradient_dim;

  // Number of dimensions of the predictions.
  int prediction_dim;

  // If true, a buffer is allocated to materialize the loss's hessian. If
  // allocated, the hessian has the same shape as the gradient.
  bool has_hessian;
};

// Set a leaf's value using one stop of the Newton–Raphson method.
// The label statistics should contain gradients + hessian values.
absl::Status SetLeafValueWithNewtonRaphsonStep(
    const proto::GradientBoostedTreesTrainingConfig& gbt_config_,
    const decision_tree::proto::LabelStatistics& label_statistics,
    decision_tree::proto::Node* node);

// Loss to optimize during the training of a GBT.
//
// The life of a loss object is as follow:
//   1. The loss is created.
//   2. Gradient, hessian and predictions buffer are created using "Shape()".
//   3. "InitialPredictions" is called to get the initial prediction of the
//      model.
//   4. The gradient of the model is updated using "UpdateGradients".
//   5. A new tree is trained to predict each gradient dimension. Tree nodes are
//      set using "SetLeafFunctor".
//   6. The prediction buffer is updated using "UpdatePredictions".
//   7. Optionally (for logging or early stopping), the "Loss" is computed.
//   8. The training stops or goes back to step 4.
//
class AbstractLoss {
 public:
  virtual ~AbstractLoss() = default;

  // Check that the loss is valid. To be called after the constructor.
  virtual absl::Status Status() const = 0;

  // Shape / number of dimensions of the gradient, prediction and hessian
  // buffers required by the loss.
  virtual LossShape Shape() const = 0;

  // Initial prediction of the model before any tree is trained. Sometime called
  // the "bias".
  virtual utils::StatusOr<std::vector<float>> InitialPredictions(
      const dataset::VerticalDataset& dataset, int label_col_idx,
      const std::vector<float>& weights) const = 0;

  // Initial predictions from a pre-aggregated label statistics.
  virtual utils::StatusOr<std::vector<float>> InitialPredictions(
      const decision_tree::proto::LabelStatistics& label_statistics) const = 0;

  // Returns true iif. the loss needs for the examples to be grouped i.e.
  // "ranking_index" will be set in "UpdateGradients" and "Loss". For example,
  // grouping can be used in ranking.
  virtual bool RequireGroupingAttribute() const { return false; }

  // The "UpdateGradients" methods compute the gradient of the loss with respect
  // to the model output. Different version of "UpdateGradients" are implemented
  // for different representation of the label.
  //
  // Currently:
  // UpdateGradients on float should be implemented for numerical labels.
  // UpdateGradients on int32_t should be implemented for categorical labels.
  // UpdateGradients on int16_t should be implemented for categorical labels
  // (only used for distributed training).

  // Updates the gradient with label stored in a vector<float>.
  virtual absl::Status UpdateGradients(
      const std::vector<float>& labels, const std::vector<float>& predictions,
      const RankingGroupsIndices* ranking_index, GradientDataRef* gradients,
      utils::RandomEngine* random,
      utils::concurrency::ThreadPool* thread_pool) const {
    return absl::InternalError("UpdateGradients not implemented");
  }

  // Updates the gradient with label stored in a vector<int32_t>.
  virtual absl::Status UpdateGradients(
      const std::vector<int32_t>& labels, const std::vector<float>& predictions,
      const RankingGroupsIndices* ranking_index, GradientDataRef* gradients,
      utils::RandomEngine* random,
      utils::concurrency::ThreadPool* thread_pool) const {
    return absl::InternalError("UpdateGradients not implemented");
  }

  // Updates the gradient with label stored in a vector<int16_t>.
  virtual absl::Status UpdateGradients(
      const std::vector<int16_t>& labels, const std::vector<float>& predictions,
      const RankingGroupsIndices* ranking_index, GradientDataRef* gradients,
      utils::RandomEngine* random,
      utils::concurrency::ThreadPool* thread_pool) const {
    return absl::InternalError("UpdateGradients not implemented");
  }

  // Updates the gradient with label stored in a VerticalDataset.
  // This method calls the UpdateGradients defined above depending on the type
  // of the label column in the VerticalDataset (currently, only support float
  // (Numerical) and int32 (Categorical)).
  absl::Status UpdateGradients(const dataset::VerticalDataset& dataset,
                               int label_col_idx,
                               const std::vector<float>& predictions,
                               const RankingGroupsIndices* ranking_index,
                               std::vector<GradientData>* gradients,
                               utils::RandomEngine* random) const;

  // Creates a function able to set the value of a leaf.
  //
  // The returned CreateSetLeafValueFunctor uses the object (this) and possibly
  // "predictions" and "gradients". All of them should outlive calls to the
  // returned function.
  //
  // "label_col_idx" corresponds to the label of the task solved by the GBT. The
  // training config (including the label index) passed to the
  // "CreateSetLeafValueFunctor" corresponds to the training label of the
  // individual tree i.e. the gradient.
  virtual decision_tree::CreateSetLeafValueFunctor SetLeafFunctor(
      const std::vector<float>& predictions,
      const std::vector<GradientData>& gradients, int label_col_idx) const = 0;

  virtual utils::StatusOr<decision_tree::SetLeafValueFromLabelStatsFunctor>
  SetLeafFunctorFromLabelStatistics() const {
    return absl::InternalError(
        "SetLeafFunctorFromLabelStatistics not implemented");
  }

  // Updates the prediction accumulator (contains the predictions of the trees
  // trained so far) with a newly learned tree (or list of trees, for
  // multi-variate gradients).
  //
  // Args:
  //   new_trees: List of newly learned trees. The i-th tree corresponds to the
  //     i-th gradient dimension.
  //   dataset: Training or validation dataset.
  //   predictions: (input+output) Predictions containing of the result of all
  //     the previous call to "InitialPredictions" and all the previous
  //     "UpdatePredictions" calls. In most cases, it is easier for this
  //     prediction accumulation not to have the activation function applied.
  //   mean_abs_prediction: (output) Return value for the mean absolute
  //     prediction of the tree. Used to plot the "impact" of each new tree.
  virtual absl::Status UpdatePredictions(
      const std::vector<const decision_tree::DecisionTree*>& new_trees,
      const dataset::VerticalDataset& dataset, std::vector<float>* predictions,
      double* mean_abs_prediction) const = 0;

  // Gets the name of the metrics returned in "secondary_metric" of the "Loss"
  // method.
  virtual std::vector<std::string> SecondaryMetricNames() const = 0;

  // The "Loss" methods compute the loss(es) for the currently accumulated
  // predictions. Like for "UpdateGradients", different version of "Loss" are
  // implemented for different representation of the label.
  //
  // See the instructions of "UpdateGradients" to see which version of "Loss"
  // should be implemented.

  absl::Status Loss(const dataset::VerticalDataset& dataset, int label_col_idx,
                    const std::vector<float>& predictions,
                    const std::vector<float>& weights,
                    const RankingGroupsIndices* ranking_index,
                    float* loss_value,
                    std::vector<float>* secondary_metric) const;

  virtual absl::Status Loss(const std::vector<int16_t>& labels,
                            const std::vector<float>& predictions,
                            const std::vector<float>& weights,
                            const RankingGroupsIndices* ranking_index,
                            float* loss_value,
                            std::vector<float>* secondary_metric,
                            utils::concurrency::ThreadPool* thread_pool) const {
    return absl::InternalError("Loss not implemented");
  }

  virtual absl::Status Loss(const std::vector<int32_t>& labels,
                            const std::vector<float>& predictions,
                            const std::vector<float>& weights,
                            const RankingGroupsIndices* ranking_index,
                            float* loss_value,
                            std::vector<float>* secondary_metric,
                            utils::concurrency::ThreadPool* thread_pool) const {
    return absl::InternalError("Loss lot implemented");
  }

  virtual absl::Status Loss(const std::vector<float>& labels,
                            const std::vector<float>& predictions,
                            const std::vector<float>& weights,
                            const RankingGroupsIndices* ranking_index,
                            float* loss_value,
                            std::vector<float>* secondary_metric,
                            utils::concurrency::ThreadPool* thread_pool) const {
    return absl::InternalError("Loss not implemented");
  }
};

// Binomial log-likelihood loss.
// Suited for binary classification.
// See "AbstractLoss" for the method documentation.
class BinomialLogLikelihoodLoss : public AbstractLoss {
 public:
  // For unit testing.
  using AbstractLoss::Loss;
  using AbstractLoss::UpdateGradients;

  BinomialLogLikelihoodLoss(
      const proto::GradientBoostedTreesTrainingConfig& gbt_config,
      model::proto::Task task, const dataset::proto::Column& label_column);

  absl::Status Status() const override;

  LossShape Shape() const override {
    return LossShape{/*.gradient_dim =*/1,
                     /*.prediction_dim =*/1,
                     /*.has_hessian =*/gbt_config_.use_hessian_gain()};
  };

  utils::StatusOr<std::vector<float>> InitialPredictions(
      const dataset::VerticalDataset& dataset, int label_col_idx,
      const std::vector<float>& weights) const override;

  virtual utils::StatusOr<std::vector<float>> InitialPredictions(
      const decision_tree::proto::LabelStatistics& label_statistics)
      const override;

  template <typename T>
  absl::Status TemplatedUpdateGradients(
      const std::vector<T>& labels, const std::vector<float>& predictions,
      const RankingGroupsIndices* ranking_index, GradientDataRef* gradients,
      utils::RandomEngine* random,
      utils::concurrency::ThreadPool* thread_pool) const;

  template <typename T>
  static void TemplatedUpdateGradientsImp(const std::vector<T>& labels,
                                          const std::vector<float>& predictions,
                                          size_t begin_example_idx,
                                          size_t end_example_idx,
                                          std::vector<float>* gradient_data,
                                          std::vector<float>* hessian_data);

  absl::Status UpdateGradients(
      const std::vector<int16_t>& labels, const std::vector<float>& predictions,
      const RankingGroupsIndices* ranking_index, GradientDataRef* gradients,
      utils::RandomEngine* random,
      utils::concurrency::ThreadPool* thread_pool) const override;

  absl::Status UpdateGradients(
      const std::vector<int32_t>& labels, const std::vector<float>& predictions,
      const RankingGroupsIndices* ranking_index, GradientDataRef* gradients,
      utils::RandomEngine* random,
      utils::concurrency::ThreadPool* thread_pool) const override;

  decision_tree::CreateSetLeafValueFunctor SetLeafFunctor(
      const std::vector<float>& predictions,
      const std::vector<GradientData>& gradients,
      int label_col_idx) const override;

  void SetLeaf(
      const dataset::VerticalDataset& train_dataset,
      const std::vector<dataset::VerticalDataset::row_t>& selected_examples,
      const std::vector<float>& weights,
      const model::proto::TrainingConfig& config,
      const model::proto::TrainingConfigLinking& config_link,
      const std::vector<float>& predictions, int label_col_idx,
      decision_tree::NodeWithChildren* node) const;

  utils::StatusOr<decision_tree::SetLeafValueFromLabelStatsFunctor>
  SetLeafFunctorFromLabelStatistics() const override {
    return [&](const decision_tree::proto::LabelStatistics& label_stats,
               decision_tree::proto::Node* node) {
      return SetLeafValueWithNewtonRaphsonStep(gbt_config_, label_stats, node);
    };
  }

  absl::Status UpdatePredictions(
      const std::vector<const decision_tree::DecisionTree*>& new_trees,
      const dataset::VerticalDataset& dataset, std::vector<float>* predictions,
      double* mean_abs_prediction) const override;

  std::vector<std::string> SecondaryMetricNames() const override;

  template <typename T>
  absl::Status TemplatedLoss(const std::vector<T>& labels,
                             const std::vector<float>& predictions,
                             const std::vector<float>& weights,
                             const RankingGroupsIndices* ranking_index,
                             float* loss_value,
                             std::vector<float>* secondary_metric,
                             utils::concurrency::ThreadPool* thread_pool) const;

  template <bool use_weights, typename T>
  static void TemplatedLossImp(const std::vector<T>& labels,
                               const std::vector<float>& predictions,
                               const std::vector<float>& weights,
                               size_t begin_example_idx, size_t end_example_idx,
                               double* __restrict sum_loss,
                               double* __restrict count_correct_predictions,
                               double* __restrict sum_weights);

  absl::Status Loss(const std::vector<int32_t>& labels,
                    const std::vector<float>& predictions,
                    const std::vector<float>& weights,
                    const RankingGroupsIndices* ranking_index,
                    float* loss_value, std::vector<float>* secondary_metric,
                    utils::concurrency::ThreadPool* thread_pool) const override;

  absl::Status Loss(const std::vector<int16_t>& labels,
                    const std::vector<float>& predictions,
                    const std::vector<float>& weights,
                    const RankingGroupsIndices* ranking_index,
                    float* loss_value, std::vector<float>* secondary_metric,
                    utils::concurrency::ThreadPool* thread_pool) const override;

 private:
  proto::GradientBoostedTreesTrainingConfig gbt_config_;
  const model::proto::Task task_;
  const dataset::proto::Column& label_column_;
};

// Mean squared Error loss.
// Suited for univariate regression.
// See "AbstractLoss" for the method documentation.
class MeanSquaredErrorLoss : public AbstractLoss {
 public:
  // For unit testing.
  using AbstractLoss::Loss;
  using AbstractLoss::UpdateGradients;

  MeanSquaredErrorLoss(
      const proto::GradientBoostedTreesTrainingConfig& gbt_config,
      model::proto::Task task, const dataset::proto::Column& label_column);

  absl::Status Status() const override;

  LossShape Shape() const override {
    return LossShape{/*.gradient_dim =*/1, /*.prediction_dim =*/1,
                     /*.has_hessian =*/false};
  };

  utils::StatusOr<std::vector<float>> InitialPredictions(
      const dataset::VerticalDataset& dataset, int label_col_idx,
      const std::vector<float>& weights) const override;

  virtual utils::StatusOr<std::vector<float>> InitialPredictions(
      const decision_tree::proto::LabelStatistics& label_statistics)
      const override;

  absl::Status UpdateGradients(
      const std::vector<float>& labels, const std::vector<float>& predictions,
      const RankingGroupsIndices* ranking_index, GradientDataRef* gradients,
      utils::RandomEngine* random,
      utils::concurrency::ThreadPool* thread_pool) const override;

  decision_tree::CreateSetLeafValueFunctor SetLeafFunctor(
      const std::vector<float>& predictions,
      const std::vector<GradientData>& gradients,
      int label_col_idx) const override;

  void SetLeaf(
      const dataset::VerticalDataset& train_dataset,
      const std::vector<dataset::VerticalDataset::row_t>& selected_examples,
      const std::vector<float>& weights,
      const model::proto::TrainingConfig& config,
      const model::proto::TrainingConfigLinking& config_link,
      const std::vector<float>& predictions, int label_col_idx,
      decision_tree::NodeWithChildren* node) const;

  utils::StatusOr<decision_tree::SetLeafValueFromLabelStatsFunctor>
  SetLeafFunctorFromLabelStatistics() const override;

  absl::Status UpdatePredictions(
      const std::vector<const decision_tree::DecisionTree*>& new_trees,
      const dataset::VerticalDataset& dataset, std::vector<float>* predictions,
      double* mean_abs_prediction) const override;

  std::vector<std::string> SecondaryMetricNames() const override;

  absl::Status Loss(const std::vector<float>& labels,
                    const std::vector<float>& predictions,
                    const std::vector<float>& weights,
                    const RankingGroupsIndices* ranking_index,
                    float* loss_value, std::vector<float>* secondary_metric,
                    utils::concurrency::ThreadPool* thread_pool) const override;

 private:
  // Effective task to solve. RMSE can be used for both RMSE and RANKING.
  model::proto::Task task_;
  proto::GradientBoostedTreesTrainingConfig gbt_config_;
};

// Multinomial log likelihood loss.
// Suited for binary and multi-class classification.
// See "AbstractLoss" for the method documentation.
class MultinomialLogLikelihoodLoss : public AbstractLoss {
 public:
  // For unit testing.
  using AbstractLoss::Loss;
  using AbstractLoss::UpdateGradients;

  MultinomialLogLikelihoodLoss(
      const proto::GradientBoostedTreesTrainingConfig& gbt_config,
      model::proto::Task task, const dataset::proto::Column& label_column);

  absl::Status Status() const override;

  LossShape Shape() const override {
    return LossShape{/*.gradient_dim =*/dimension_,
                     /*.prediction_dim =*/dimension_,
                     /*.has_hessian =*/gbt_config_.use_hessian_gain()};
  };

  utils::StatusOr<std::vector<float>> InitialPredictions(
      const dataset::VerticalDataset& dataset, int label_col_idx,
      const std::vector<float>& weights) const override;

  virtual utils::StatusOr<std::vector<float>> InitialPredictions(
      const decision_tree::proto::LabelStatistics& label_statistics)
      const override;

  template <typename T>
  absl::Status TemplatedUpdateGradients(
      const std::vector<T>& labels, const std::vector<float>& predictions,
      const RankingGroupsIndices* ranking_index, GradientDataRef* gradients,
      utils::RandomEngine* random,
      utils::concurrency::ThreadPool* thread_pool) const;

  absl::Status UpdateGradients(
      const std::vector<int16_t>& labels, const std::vector<float>& predictions,
      const RankingGroupsIndices* ranking_index, GradientDataRef* gradients,
      utils::RandomEngine* random,
      utils::concurrency::ThreadPool* thread_pool) const override;

  absl::Status UpdateGradients(
      const std::vector<int32_t>& labels, const std::vector<float>& predictions,
      const RankingGroupsIndices* ranking_index, GradientDataRef* gradients,
      utils::RandomEngine* random,
      utils::concurrency::ThreadPool* thread_pool) const override;

  decision_tree::CreateSetLeafValueFunctor SetLeafFunctor(
      const std::vector<float>& predictions,
      const std::vector<GradientData>& gradients,
      int label_col_idx) const override;

  void SetLeaf(
      const dataset::VerticalDataset& train_dataset,
      const std::vector<dataset::VerticalDataset::row_t>& selected_examples,
      const std::vector<float>& weights,
      const model::proto::TrainingConfig& config,
      const model::proto::TrainingConfigLinking& config_link,
      const std::vector<float>& predictions, int label_col_idx,
      decision_tree::NodeWithChildren* node) const;

  utils::StatusOr<decision_tree::SetLeafValueFromLabelStatsFunctor>
  SetLeafFunctorFromLabelStatistics() const override {
    return [&](const decision_tree::proto::LabelStatistics& label_stats,
               decision_tree::proto::Node* node) {
      return SetLeafValueWithNewtonRaphsonStep(gbt_config_, label_stats, node);
    };
  }

  absl::Status UpdatePredictions(
      const std::vector<const decision_tree::DecisionTree*>& new_trees,
      const dataset::VerticalDataset& dataset, std::vector<float>* predictions,
      double* mean_abs_prediction) const override;

  std::vector<std::string> SecondaryMetricNames() const override;

  template <typename T>
  absl::Status TemplatedLoss(const std::vector<T>& labels,
                             const std::vector<float>& predictions,
                             const std::vector<float>& weights,
                             const RankingGroupsIndices* ranking_index,
                             float* loss_value,
                             std::vector<float>* secondary_metric,
                             utils::concurrency::ThreadPool* thread_pool) const;

  absl::Status Loss(const std::vector<int32_t>& labels,
                    const std::vector<float>& predictions,
                    const std::vector<float>& weights,
                    const RankingGroupsIndices* ranking_index,
                    float* loss_value, std::vector<float>* secondary_metric,
                    utils::concurrency::ThreadPool* thread_pool) const override;

  absl::Status Loss(const std::vector<int16_t>& labels,
                    const std::vector<float>& predictions,
                    const std::vector<float>& weights,
                    const RankingGroupsIndices* ranking_index,
                    float* loss_value, std::vector<float>* secondary_metric,
                    utils::concurrency::ThreadPool* thread_pool) const override;

 private:
  int dimension_;
  proto::GradientBoostedTreesTrainingConfig gbt_config_;
  const model::proto::Task task_;
  const dataset::proto::Column& label_column_;
};

// Normalized Discounted Cumulative Gain loss.
// Suited for ranking.
// See "AbstractLoss" for the method documentation.
class NDCGLoss : public AbstractLoss {
 public:
  // For unit testing.
  using AbstractLoss::Loss;
  using AbstractLoss::UpdateGradients;

  NDCGLoss(const proto::GradientBoostedTreesTrainingConfig& gbt_config,
           model::proto::Task task, const dataset::proto::Column& label_column);

  absl::Status Status() const override;

  bool RequireGroupingAttribute() const override { return true; }

  LossShape Shape() const override {
    return LossShape{/*.gradient_dim =*/1, /*.prediction_dim =*/1,
                     /*.has_hessian =*/true};
  };

  utils::StatusOr<std::vector<float>> InitialPredictions(
      const dataset::VerticalDataset& dataset, int label_col_idx,
      const std::vector<float>& weights) const override;

  virtual utils::StatusOr<std::vector<float>> InitialPredictions(
      const decision_tree::proto::LabelStatistics& label_statistics)
      const override;

  absl::Status UpdateGradients(
      const std::vector<float>& labels, const std::vector<float>& predictions,
      const RankingGroupsIndices* ranking_index, GradientDataRef* gradients,
      utils::RandomEngine* random,
      utils::concurrency::ThreadPool* thread_pool) const override;

  decision_tree::CreateSetLeafValueFunctor SetLeafFunctor(
      const std::vector<float>& predictions,
      const std::vector<GradientData>& gradients,
      int label_col_idx) const override;

  static void SetLeafStatic(
      const dataset::VerticalDataset& train_dataset,
      const std::vector<dataset::VerticalDataset::row_t>& selected_examples,
      const std::vector<float>& weights,
      const model::proto::TrainingConfig& config,
      const model::proto::TrainingConfigLinking& config_link,
      const std::vector<float>& predictions,
      const proto::GradientBoostedTreesTrainingConfig& gbt_config,
      const std::vector<GradientData>& gradients, int label_col_idx,
      decision_tree::NodeWithChildren* node);

  absl::Status UpdatePredictions(
      const std::vector<const decision_tree::DecisionTree*>& new_trees,
      const dataset::VerticalDataset& dataset, std::vector<float>* predictions,
      double* mean_abs_prediction) const override;

  std::vector<std::string> SecondaryMetricNames() const override;

  absl::Status Loss(const std::vector<float>& labels,
                    const std::vector<float>& predictions,
                    const std::vector<float>& weights,
                    const RankingGroupsIndices* ranking_index,
                    float* loss_value, std::vector<float>* secondary_metric,
                    utils::concurrency::ThreadPool* thread_pool) const override;

 private:
  proto::GradientBoostedTreesTrainingConfig gbt_config_;
  model::proto::Task task_;
};

// Cross Entropy Normalized Discounted Cumulative Gain (XE-NDCG) loss.
// Suited for ranking.
// See "AbstractLoss" for the method documentation.
class CrossEntropyNDCGLoss : public AbstractLoss {
 public:
  // For unit testing.
  using AbstractLoss::Loss;
  using AbstractLoss::UpdateGradients;

  CrossEntropyNDCGLoss(
      const proto::GradientBoostedTreesTrainingConfig& gbt_config,
      model::proto::Task task, const dataset::proto::Column& label_column);

  absl::Status Status() const override;

  bool RequireGroupingAttribute() const override { return true; }

  LossShape Shape() const override {
    return LossShape{/*.gradient_dim =*/1, /*.prediction_dim =*/1,
                     /*.has_hessian =*/true};
  };

  utils::StatusOr<std::vector<float>> InitialPredictions(
      const dataset::VerticalDataset& dataset, int label_col_idx,
      const std::vector<float>& weights) const override;

  virtual utils::StatusOr<std::vector<float>> InitialPredictions(
      const decision_tree::proto::LabelStatistics& label_statistics)
      const override;

  absl::Status UpdateGradients(
      const std::vector<float>& labels, const std::vector<float>& predictions,
      const RankingGroupsIndices* ranking_index, GradientDataRef* gradients,
      utils::RandomEngine* random,
      utils::concurrency::ThreadPool* thread_pool) const override;

  decision_tree::CreateSetLeafValueFunctor SetLeafFunctor(
      const std::vector<float>& predictions,
      const std::vector<GradientData>& gradients,
      int label_col_idx) const override;

  absl::Status UpdatePredictions(
      const std::vector<const decision_tree::DecisionTree*>& new_trees,
      const dataset::VerticalDataset& dataset, std::vector<float>* predictions,
      double* mean_abs_prediction) const override;

  std::vector<std::string> SecondaryMetricNames() const override;

  absl::Status Loss(const std::vector<float>& labels,
                    const std::vector<float>& predictions,
                    const std::vector<float>& weights,
                    const RankingGroupsIndices* ranking_index,
                    float* loss_value, std::vector<float>* secondary_metric,
                    utils::concurrency::ThreadPool* thread_pool) const override;

 private:
  proto::GradientBoostedTreesTrainingConfig gbt_config_;
  model::proto::Task task_;
};

// Creates a training loss.
utils::StatusOr<std::unique_ptr<AbstractLoss>> CreateLoss(
    proto::Loss loss, model::proto::Task task,
    const dataset::proto::Column& label_column,
    const proto::GradientBoostedTreesTrainingConfig& config);

}  // namespace gradient_boosted_trees
}  // namespace model
}  // namespace yggdrasil_decision_forests

#endif  // YGGDRASIL_DECISION_FORESTS_LEARNER_GRADIENT_BOOSTED_TREES_LOSS_H_
