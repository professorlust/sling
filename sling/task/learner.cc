// Copyright 2017 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "sling/task/learner.h"

#include <unistd.h>

#include "sling/task/task.h"

namespace sling {
namespace task {

using namespace myelin;

void LearnerTask::Train(Task *task, myelin::Network *model) {
  // Get training parameters.
  task->Fetch("epochs", &epochs_);
  task->Fetch("report_interval", &report_interval_);
  task->Fetch("rampup", &rampup_);

  // Initialize statistics counters.
  num_workers_ = task->GetCounter("workers");
  num_epochs_total_ = task->GetCounter("epochs_total");
  num_epochs_completed_ = task->GetCounter("epochs_completed");

  epoch_ = 0;
  num_epochs_total_->Increment(epochs_);

  // Start training threads.
  int threads = task->Get("workers", jit::CPU::Processors());
  WorkerPool pool;
  pool.Start(threads, [this, model](int index) {
    for (int i = 0; i < index * rampup_ && !done_; ++i) sleep(1);
    num_workers_->Increment();
    if (!done_) Worker(index, model);
  });

  // Evaluate model on regular intervals. The workers signal when it is time
  // for the next eval round or training has completed.
  while (!done_) {
    // Wait for next eval or completion.
    {
      std::unique_lock<std::mutex> lock(eval_mu_);
      eval_model_.wait(lock);
    }

    // Run evaluation.
    Evaluate(epoch_, model);
  }

  // Run final evaluation.
  Evaluate(epoch_, model);

  // Wait until workers complete.
  pool.Join();
}

bool LearnerTask::EpochCompleted() {
  num_epochs_completed_->Increment();
  int64 current_epoch = ++epoch_;
  if (current_epoch >= epochs_) done_ = true;
  bool eval = current_epoch % report_interval_ == 0;
  if (eval || done_) {
    std::unique_lock<std::mutex> lock(eval_mu_);
    eval_model_.notify_one();
  }
  return done_;
}

Optimizer *GetOptimizer(Task *task) {
  const string &type = task->Get("optimizer", "sgd");
  float lr = task->Get("learning_rate", 0.01);
  float decay = task->Get("learning_rate_decay", 1.0);
  float clip = task->Get("clipping", 0.0);
  if (type == "sgd") {
    GradientDescentOptimizer *sgd = new GradientDescentOptimizer();
    sgd->set_learning_rate(lr);
    sgd->set_decay(decay);
    sgd->set_clipping_threshold(clip);
    sgd->set_lambda(task->Get("regularization", 0.0));
    return sgd;
  } else if (type == "momentum") {
    MomentumOptimizer *momentum = new MomentumOptimizer();
    momentum->set_learning_rate(lr);
    momentum->set_decay(decay);
    momentum->set_clipping_threshold(clip);
    momentum->set_momentum(task->Get("momentum", 0.9));
    return momentum;
  } else if (type == "adam") {
    AdamOptimizer *adam = new AdamOptimizer();
    adam->set_learning_rate(lr);
    adam->set_decay(decay);
    adam->set_clipping_threshold(clip);
    adam->set_beta1(task->Get("beta1", 0.9));
    adam->set_beta2(task->Get("beta2", 0.99));
    adam->set_epsilon(task->Get("epsilon", 1e-8));
    return adam;
  } else {
    LOG(FATAL) << "Unknown optimizer type: " << type;
    return nullptr;
  }
}

}  // namespace task
}  // namespace sling
