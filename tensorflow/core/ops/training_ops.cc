/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/shape_inference.h"

namespace tensorflow {

typedef shape_inference::Dimension Dimension;
typedef shape_inference::InferenceContext InferenceContext;
typedef shape_inference::Shape Shape;
static constexpr auto kUnknownDim = InferenceContext::kUnknownDim;

static Status ApplyGradientDescentShapeFn(InferenceContext* c) {
  const Shape* unused;
  const Shape* s = c->input(0);                              // var
  TF_RETURN_IF_ERROR(c->WithRank(c->input(1), 0, &unused));  // alpha
  TF_RETURN_IF_ERROR(c->Merge(s, c->input(2), &s));          // delta
  c->set_output(0, s);
  return Status::OK();
}

REGISTER_OP("ApplyGradientDescent")
    .Input("var: Ref(T)")
    .Input("alpha: T")
    .Input("delta: T")
    .Output("out: Ref(T)")
    .Attr("T: numbertype")
    .Attr("use_locking: bool = false")
    .SetShapeFn(OpShapeInferenceFn(ApplyGradientDescentShapeFn))
    .Doc(R"doc(
Update '*var' by subtracting 'alpha' * 'delta' from it.

var: Should be from a Variable().
alpha: Scaling factor. Must be a scalar.
delta: The change.
out: Same as "var".
use_locking: If `True`, the subtraction will be protected by a lock;
  otherwise the behavior is undefined, but may exhibit less contention.
)doc");

static Status ApplyProxiimalGradientDescentShapeFn(InferenceContext* c) {
  const Shape* unused;
  const Shape* s = c->input(0);                              // var
  TF_RETURN_IF_ERROR(c->WithRank(c->input(1), 0, &unused));  // alpha
  TF_RETURN_IF_ERROR(c->WithRank(c->input(2), 0, &unused));  // l1
  TF_RETURN_IF_ERROR(c->WithRank(c->input(3), 0, &unused));  // l2
  TF_RETURN_IF_ERROR(c->Merge(s, c->input(4), &s));          // delta
  c->set_output(0, s);
  return Status::OK();
}

REGISTER_OP("ApplyProximalGradientDescent")
    .Input("var: Ref(T)")
    .Input("alpha: T")
    .Input("l1: T")
    .Input("l2: T")
    .Input("delta: T")
    .Output("out: Ref(T)")
    .Attr("T: numbertype")
    .Attr("use_locking: bool = false")
    .SetShapeFn(OpShapeInferenceFn(ApplyProxiimalGradientDescentShapeFn))
    .Doc(R"doc(
Update '*var' as FOBOS algorithm with fixed learning rate.
prox_v = var - alpha * delta
var = sign(prox_v)/(1+alpha*l2) * max{|prox_v|-alpha*l1,0}

var: Should be from a Variable().
alpha: Scaling factor. Must be a scalar.
l1: L1 regularization. Must be a scalar.
l2: L2 regularization. Must be a scalar.
delta: The change.
out: Same as "var".
use_locking: If True, the subtraction will be protected by a lock;
  otherwise the behavior is undefined, but may exhibit less contention.
)doc");

REGISTER_OP("SparseApplyProximalGradientDescent")
    .Input("var: Ref(T)")
    .Input("alpha: T")
    .Input("l1: T")
    .Input("l2: T")
    .Input("grad: T")
    .Input("indices: Tindices")
    .Output("out: Ref(T)")
    .Attr("T: numbertype")
    .Attr("Tindices: {int32, int64}")
    .Attr("use_locking: bool = false")
    .Doc(R"doc(
Sparse update '*var' as FOBOS algorithm with fixed learning rate.

That is for rows we have grad for, we update var as follows:
prox_v = var - alpha * grad
var = sign(prox_v)/(1+alpha*l2) * max{|prox_v|-alpha*l1,0}

var: Should be from a Variable().
alpha: Scaling factor. Must be a scalar.
l1: L1 regularization. Must be a scalar.
l2: L2 regularization. Must be a scalar.
grad: The gradient.
indices: A vector of indices into the first dimension of var and accum.
out: Same as "var".
use_locking: If True, the subtraction will be protected by a lock;
  otherwise the behavior is undefined, but may exhibit less contention.
)doc");
static Status ApplyAdadeltaShapeFn(InferenceContext* c) {
  const Shape* unused;
  const Shape* s = c->input(0);                              // var
  TF_RETURN_IF_ERROR(c->Merge(s, c->input(1), &s));          // accum
  TF_RETURN_IF_ERROR(c->Merge(s, c->input(2), &s));          // accum update
  TF_RETURN_IF_ERROR(c->WithRank(c->input(3), 0, &unused));  // lr
  TF_RETURN_IF_ERROR(c->WithRank(c->input(4), 0, &unused));  // rho
  TF_RETURN_IF_ERROR(c->WithRank(c->input(5), 0, &unused));  // epsilon
  TF_RETURN_IF_ERROR(c->Merge(s, c->input(6), &s));          // grad
  c->set_output(0, s);
  return Status::OK();
}

REGISTER_OP("ApplyAdadelta")
    .Input("var: Ref(T)")
    .Input("accum: Ref(T)")
    .Input("accum_update: Ref(T)")
    .Input("lr: T")
    .Input("rho: T")
    .Input("epsilon: T")
    .Input("grad: T")
    .Output("out: Ref(T)")
    .Attr("T: numbertype")
    .Attr("use_locking: bool = false")
    .SetShapeFn(OpShapeInferenceFn(ApplyAdadeltaShapeFn))
    .Doc(R"doc(
Update '*var' according to the adadelta scheme.

accum = rho() * accum + (1 - rho()) * grad.square();
update = (update_accum + epsilon).sqrt() * (accum + epsilon()).rsqrt() * grad;
update_accum = rho() * update_accum + (1 - rho()) * update.square();
var -= update;

var: Should be from a Variable().
accum: Should be from a Variable().
accum_update: Should be from a Variable().
lr: Scaling factor. Must be a scalar.
rho: Decay factor. Must be a scalar.
epsilon: Constant factor. Must be a scalar.
grad: The gradient.
out: Same as "var".
use_locking: If True, updating of the var, accum and update_accum tensors will be protected by
a lock; otherwise the behavior is undefined, but may exhibit less contention.
)doc");

REGISTER_OP("SparseApplyAdadelta")
    .Input("var: Ref(T)")
    .Input("accum: Ref(T)")
    .Input("accum_update: Ref(T)")
    .Input("lr: T")
    .Input("rho: T")
    .Input("epsilon: T")
    .Input("grad: T")
    .Input("indices: Tindices")
    .Output("out: Ref(T)")
    .Attr("T: numbertype")
    .Attr("Tindices: {int32, int64}")
    .Attr("use_locking: bool = false")
    .Doc(R"doc(
var: Should be from a Variable().
accum: Should be from a Variable().
accum_update:: Should be from a Variable().
lr: Learning rate. Must be a scalar.
rho: Decay factor. Must be a scalar.
epsilon: Constant factor. Must be a scalar.
grad: The gradient.
indices: A vector of indices into the first dimension of var and accum.
out: Same as "var".
use_locking: If True, updating of the var and accum tensors will be protected by
a lock; otherwise the behavior is undefined, but may exhibit less contention.
)doc");
static Status ApplyAdagradShapeFn(InferenceContext* c) {
  const Shape* unused;
  const Shape* s = c->input(0);                              // var
  TF_RETURN_IF_ERROR(c->Merge(s, c->input(1), &s));          // accum
  TF_RETURN_IF_ERROR(c->WithRank(c->input(2), 0, &unused));  // lr
  TF_RETURN_IF_ERROR(c->Merge(s, c->input(3), &s));          // grad
  c->set_output(0, s);
  return Status::OK();
}

REGISTER_OP("ApplyAdagrad")
    .Input("var: Ref(T)")
    .Input("accum: Ref(T)")
    .Input("lr: T")
    .Input("grad: T")
    .Output("out: Ref(T)")
    .Attr("T: numbertype")
    .Attr("use_locking: bool = false")
    .SetShapeFn(OpShapeInferenceFn(ApplyAdagradShapeFn))

    .Doc(R"doc(
Update '*var' according to the adagrad scheme.

accum += grad * grad
var -= lr * grad * (1 / sqrt(accum))

var: Should be from a Variable().
accum: Should be from a Variable().
lr: Scaling factor. Must be a scalar.
grad: The gradient.
out: Same as "var".
use_locking: If `True`, updating of the var and accum tensors will be protected
  by a lock; otherwise the behavior is undefined, but may exhibit less
  contention.
)doc");
static Status ApplyProximalAdagradShapeFn(InferenceContext* c) {
  const Shape* unused;
  const Shape* s = c->input(0);                              // var
  TF_RETURN_IF_ERROR(c->Merge(s, c->input(1), &s));          // accum
  TF_RETURN_IF_ERROR(c->WithRank(c->input(2), 0, &unused));  // lr
  TF_RETURN_IF_ERROR(c->WithRank(c->input(3), 0, &unused));  // l1
  TF_RETURN_IF_ERROR(c->WithRank(c->input(4), 0, &unused));  // l2
  TF_RETURN_IF_ERROR(c->Merge(s, c->input(5), &s));          // grad
  c->set_output(0, s);
  return Status::OK();
}

REGISTER_OP("ApplyProximalAdagrad")
    .Input("var: Ref(T)")
    .Input("accum: Ref(T)")
    .Input("lr: T")
    .Input("l1: T")
    .Input("l2: T")
    .Input("grad: T")
    .Output("out: Ref(T)")
    .Attr("T: numbertype")
    .Attr("use_locking: bool = false")
    .SetShapeFn(OpShapeInferenceFn(ApplyProximalAdagradShapeFn))

    .Doc(R"doc(
Update '*var' and '*accum' according to FOBOS with Adagrad learning rate.
accum += grad * grad
prox_v = var - lr * grad * (1 / sqrt(accum))
var = sign(prox_v)/(1+lr*l2) * max{|prox_v|-lr*l1,0}

var: Should be from a Variable().
accum: Should be from a Variable().
grad: The gradient.
lr: Scaling factor. Must be a scalar.
l1: L1 regularization. Must be a scalar.
l2: L2 regularization. Must be a scalar.
out: Same as "var".
use_locking: If True, updating of the var and accum tensors will be protected by
a lock; otherwise the behavior is undefined, but may exhibit less contention.
)doc");

REGISTER_OP("SparseApplyAdagrad")
    .Input("var: Ref(T)")
    .Input("accum: Ref(T)")
    .Input("lr: T")
    .Input("grad: T")
    .Input("indices: Tindices")
    .Output("out: Ref(T)")
    .Attr("T: numbertype")
    .Attr("Tindices: {int32, int64}")
    .Attr("use_locking: bool = false")
    .Doc(R"doc(
Update relevant entries in '*var' and '*accum' according to the adagrad scheme.

That is for rows we have grad for, we update var and accum as follows:
accum += grad * grad
var -= lr * grad * (1 / sqrt(accum))

var: Should be from a Variable().
accum: Should be from a Variable().
lr: Learning rate. Must be a scalar.
grad: The gradient.
indices: A vector of indices into the first dimension of var and accum.
out: Same as "var".
use_locking: If `True`, updating of the var and accum tensors will be protected
  by a lock; otherwise the behavior is undefined, but may exhibit less
  contention.
)doc");

REGISTER_OP("SparseApplyProximalAdagrad")
    .Input("var: Ref(T)")
    .Input("accum: Ref(T)")
    .Input("lr: T")
    .Input("l1: T")
    .Input("l2: T")
    .Input("grad: T")
    .Input("indices: Tindices")
    .Output("out: Ref(T)")
    .Attr("T: numbertype")
    .Attr("Tindices: {int32, int64}")
    .Attr("use_locking: bool = false")
    .Doc(R"doc(
Sparse update entries in '*var' and '*accum' according to FOBOS algorithm.

That is for rows we have grad for, we update var and accum as follows:
accum += grad * grad
prox_v = var
prox_v -= lr * grad * (1 / sqrt(accum))
var = sign(prox_v)/(1+lr*l2) * max{|prox_v|-lr*l1,0}

var: Should be from a Variable().
accum: Should be from a Variable().
lr: Learning rate. Must be a scalar.
l1: L1 regularization. Must be a scalar.
l2: L2 regularization. Must be a scalar.
grad: The gradient.
indices: A vector of indices into the first dimension of var and accum.
out: Same as "var".
use_locking: If True, updating of the var and accum tensors will be protected by
a lock; otherwise the behavior is undefined, but may exhibit less contention.
)doc");

static Status ApplyFtrlShapeFn(InferenceContext* c) {
  const Shape* unused;
  const Shape* s = c->input(0);                              // var
  TF_RETURN_IF_ERROR(c->Merge(s, c->input(1), &s));          // accum
  TF_RETURN_IF_ERROR(c->Merge(s, c->input(2), &s));          // linear
  TF_RETURN_IF_ERROR(c->Merge(s, c->input(3), &s));          // grad
  TF_RETURN_IF_ERROR(c->WithRank(c->input(4), 0, &unused));  // lr
  TF_RETURN_IF_ERROR(c->WithRank(c->input(5), 0, &unused));  // l1
  TF_RETURN_IF_ERROR(c->WithRank(c->input(6), 0, &unused));  // l2
  TF_RETURN_IF_ERROR(c->WithRank(c->input(7), 0, &unused));  // lr_power
  c->set_output(0, s);
  return Status::OK();
}

REGISTER_OP("ApplyFtrl")
    .Input("var: Ref(T)")
    .Input("accum: Ref(T)")
    .Input("linear: Ref(T)")
    .Input("grad: T")
    .Input("lr: T")
    .Input("l1: T")
    .Input("l2: T")
    .Input("lr_power: T")
    .Output("out: Ref(T)")
    .Attr("T: numbertype")
    .Attr("use_locking: bool = false")
    .SetShapeFn(OpShapeInferenceFn(ApplyFtrlShapeFn))

    .Doc(R"doc(
Update '*var' according to the Ftrl-proximal scheme.

accum_new = accum + grad * grad
linear += grad + (accum_new^(-lr_power) - accum^(-lr_power)) / lr * var
quadratic = 1.0 / (accum_new^(lr_power) * lr) + 2 * l2
var = (sign(linear) * l1 - linear) / quadratic if |linear| > l1 else 0.0
accum = accum_new

var: Should be from a Variable().
accum: Should be from a Variable().
linear: Should be from a Variable().
grad: The gradient.
lr: Scaling factor. Must be a scalar.
l1: L1 regulariation. Must be a scalar.
l2: L2 regulariation. Must be a scalar.
lr_power: Scaling factor. Must be a scalar.
out: Same as "var".
use_locking: If `True`, updating of the var and accum tensors will be protected
  by a lock; otherwise the behavior is undefined, but may exhibit less
  contention.
)doc");

REGISTER_OP("SparseApplyFtrl")
    .Input("var: Ref(T)")
    .Input("accum: Ref(T)")
    .Input("linear: Ref(T)")
    .Input("grad: T")
    .Input("indices: Tindices")
    .Input("lr: T")
    .Input("l1: T")
    .Input("l2: T")
    .Input("lr_power: T")
    .Output("out: Ref(T)")
    .Attr("T: numbertype")
    .Attr("Tindices: {int32, int64}")
    .Attr("use_locking: bool = false")
    .Doc(R"doc(
Update relevant entries in '*var' according to the Ftrl-proximal scheme.

That is for rows we have grad for, we update var, accum and linear as follows:
accum_new = accum + grad * grad
linear += grad + (accum_new^(-lr_power) - accum^(-lr_power)) / lr * var
quadratic = 1.0 / (accum_new^(lr_power) * lr) + 2 * l2
var = (sign(linear) * l1 - linear) / quadratic if |linear| > l1 else 0.0
accum = accum_new

var: Should be from a Variable().
accum: Should be from a Variable().
linear: Should be from a Variable().
grad: The gradient.
indices: A vector of indices into the first dimension of var and accum.
lr: Scaling factor. Must be a scalar.
l1: L1 regularization. Must be a scalar.
l2: L2 regularization. Must be a scalar.
lr_power: Scaling factor. Must be a scalar.
out: Same as "var".
use_locking: If `True`, updating of the var and accum tensors will be protected
  by a lock; otherwise the behavior is undefined, but may exhibit less
  contention.
)doc");

static Status ApplyMomentumShapeFn(InferenceContext* c) {
  const Shape* unused;
  const Shape* s = c->input(0);                              // var
  TF_RETURN_IF_ERROR(c->Merge(s, c->input(1), &s));          // accum
  TF_RETURN_IF_ERROR(c->WithRank(c->input(2), 0, &unused));  // lr
  TF_RETURN_IF_ERROR(c->Merge(s, c->input(3), &s));          // grad
  TF_RETURN_IF_ERROR(c->WithRank(c->input(4), 0, &unused));  // momentum
  c->set_output(0, s);
  return Status::OK();
}

REGISTER_OP("ApplyMomentum")
    .Input("var: Ref(T)")
    .Input("accum: Ref(T)")
    .Input("lr: T")
    .Input("grad: T")
    .Input("momentum: T")
    .Output("out: Ref(T)")
    .Attr("T: numbertype")
    .Attr("use_locking: bool = false")
    .SetShapeFn(OpShapeInferenceFn(ApplyMomentumShapeFn))
    .Doc(R"doc(
Update '*var' according to the momentum scheme.

accum = accum * momentum + grad
var -= lr * accum

var: Should be from a Variable().
accum: Should be from a Variable().
lr: Scaling factor. Must be a scalar.
grad: The gradient.
momentum: Momentum. Must be a scalar.
out: Same as "var".
use_locking: If `True`, updating of the var and accum tensors will be protected
  by a lock; otherwise the behavior is undefined, but may exhibit less
  contention.
)doc");

REGISTER_OP("SparseApplyMomentum")
    .Input("var: Ref(T)")
    .Input("accum: Ref(T)")
    .Input("lr: T")
    .Input("grad: T")
    .Input("indices: Tindices")
    .Input("momentum: T")
    .Output("out: Ref(T)")
    .Attr("T: numbertype")
    .Attr("Tindices: {int32, int64}")
    .Attr("use_locking: bool = false")
    .Doc(R"doc(
Update relevant entries in '*var' and '*accum' according to the momentum scheme.

That is for rows we have grad for, we update var and accum as follows:

accum = accum * momentum + grad
var -= lr * accum

var: Should be from a Variable().
accum: Should be from a Variable().
lr: Learning rate. Must be a scalar.
grad: The gradient.
indices: A vector of indices into the first dimension of var and accum.
momentum: Momentum. Must be a scalar.
out: Same as "var".
use_locking: If `True`, updating of the var and accum tensors will be protected
  by a lock; otherwise the behavior is undefined, but may exhibit less
  contention.
)doc");

static Status ApplyAdamShapeFn(InferenceContext* c) {
  const Shape* unused;
  const Shape* s = c->input(0);                              // var
  TF_RETURN_IF_ERROR(c->Merge(s, c->input(1), &s));          // m
  TF_RETURN_IF_ERROR(c->Merge(s, c->input(2), &s));          // v
  TF_RETURN_IF_ERROR(c->WithRank(c->input(3), 0, &unused));  // beta1_power
  TF_RETURN_IF_ERROR(c->WithRank(c->input(4), 0, &unused));  // beta2_power
  TF_RETURN_IF_ERROR(c->WithRank(c->input(5), 0, &unused));  // lr
  TF_RETURN_IF_ERROR(c->WithRank(c->input(6), 0, &unused));  // beta1
  TF_RETURN_IF_ERROR(c->WithRank(c->input(7), 0, &unused));  // beta2
  TF_RETURN_IF_ERROR(c->WithRank(c->input(8), 0, &unused));  // epsilon
  TF_RETURN_IF_ERROR(c->Merge(s, c->input(9), &s));          // grad
  c->set_output(0, s);
  return Status::OK();
}

REGISTER_OP("ApplyAdam")
    .Input("var: Ref(T)")
    .Input("m: Ref(T)")
    .Input("v: Ref(T)")
    .Input("beta1_power: T")
    .Input("beta2_power: T")
    .Input("lr: T")
    .Input("beta1: T")
    .Input("beta2: T")
    .Input("epsilon: T")
    .Input("grad: T")
    .Output("out: Ref(T)")
    .Attr("T: numbertype")
    .Attr("use_locking: bool = false")
    .SetShapeFn(OpShapeInferenceFn(ApplyAdamShapeFn))
    .Doc(R"doc(
Update '*var' according to the Adam algorithm.

lr_t <- learning_rate * sqrt(1 - beta2^t) / (1 - beta1^t)
m_t <- beta1 * m_{t-1} + (1 - beta1) * g_t
v_t <- beta2 * v_{t-1} + (1 - beta2) * g_t * g_t
variable <- variable - lr_t * m_t / (sqrt(v_t) + epsilon)

var: Should be from a Variable().
m: Should be from a Variable().
v: Should be from a Variable().
beta1_power: Must be a scalar.
beta2_power: Must be a scalar.
lr: Scaling factor. Must be a scalar.
beta1: Momentum factor. Must be a scalar.
beta2: Momentum factor. Must be a scalar.
epsilon: Ridge term. Must be a scalar.
grad: The gradient.
out: Same as "var".
use_locking: If `True`, updating of the var, m, and v tensors will be protected
  by a lock; otherwise the behavior is undefined, but may exhibit less
  contention.
)doc");

static Status ApplyRMSPropShapeFn(InferenceContext* c) {
  const Shape* unused;
  const Shape* s = c->input(0);                              // var
  TF_RETURN_IF_ERROR(c->Merge(s, c->input(1), &s));          // ms
  TF_RETURN_IF_ERROR(c->Merge(s, c->input(2), &s));          // mom
  TF_RETURN_IF_ERROR(c->WithRank(c->input(3), 0, &unused));  // lr
  TF_RETURN_IF_ERROR(c->WithRank(c->input(4), 0, &unused));  // rho
  TF_RETURN_IF_ERROR(c->WithRank(c->input(5), 0, &unused));  // momentum
  TF_RETURN_IF_ERROR(c->WithRank(c->input(6), 0, &unused));  // epsilon
  TF_RETURN_IF_ERROR(c->Merge(s, c->input(7), &s));          // grad
  c->set_output(0, s);
  return Status::OK();
}

REGISTER_OP("ApplyRMSProp")
    .Input("var: Ref(T)")
    .Input("ms: Ref(T)")
    .Input("mom: Ref(T)")
    .Input("lr: T")
    .Input("rho: T")
    .Input("momentum: T")
    .Input("epsilon: T")
    .Input("grad: T")
    .Output("out: Ref(T)")
    .Attr("T: numbertype")
    .Attr("use_locking: bool = false")
    .SetShapeFn(OpShapeInferenceFn(ApplyRMSPropShapeFn))
    .Doc(R"doc(
Update '*var' according to the RMSProp algorithm.
Note that in dense implement of this algorithm, ms and mom will 
update even if the grad is zero, but in this sparse implement, ms 
and mom will not update in iterations the grad is zero.

mean_square = decay * mean_square + (1-decay) * gradient ** 2
Delta = learning_rate * gradient / sqrt(mean_square + epsilon)

ms <- rho * ms_{t-1} + (1-rho) * grad * grad
mom <- momentum * mom_{t-1} + lr * grad / sqrt(ms + epsilon)
var <- var - mom

var: Should be from a Variable().
ms: Should be from a Variable().
mom: Should be from a Variable().
lr: Scaling factor. Must be a scalar.
epsilon: Ridge term. Must be a scalar.
rho: Decay rate. Must be a scalar.
grad: The gradient.
out: Same as "var".
use_locking: If `True`, updating of the var, m, and v tensors will be protected
  by a lock; otherwise the behavior is undefined, but may exhibit less
  contention.
)doc");

REGISTER_OP("SparseApplyRMSProp")
    .Input("var: Ref(T)")
    .Input("ms: Ref(T)")
    .Input("mom: Ref(T)")
    .Input("lr: T")
    .Input("rho: T")
    .Input("momentum: T")
    .Input("epsilon: T")
    .Input("grad: T")
    .Input("indices: Tindices")
    .Output("out: Ref(T)")
    .Attr("T: numbertype")
    .Attr("Tindices: {int32, int64}")
    .Attr("use_locking: bool = false")
    .Doc(R"doc(
Update '*var' according to the RMSProp algorithm.
Note that in dense implement of this algorithm, ms and mom will 
update even if the grad is zero, but in this sparse implement, ms 
and mom will not update in iterations the grad is zero.

mean_square = decay * mean_square + (1-decay) * gradient ** 2
Delta = learning_rate * gradient / sqrt(mean_square + epsilon)

ms <- rho * ms_{t-1} + (1-rho) * grad * grad
mom <- momentum * mom_{t-1} + lr * grad / sqrt(ms + epsilon)
var <- var - mom

var: Should be from a Variable().
ms: Should be from a Variable().
mom: Should be from a Variable().
lr: Scaling factor. Must be a scalar.
epsilon: Ridge term. Must be a scalar.
rho: Decay rate. Must be a scalar.
grad: The gradient.
indices: A vector of indices into the first dimension of var, ms and mom.
out: Same as "var".
use_locking: If `True`, updating of the var, m, and v tensors will be protected
  by a lock; otherwise the behavior is undefined, but may exhibit less
  contention.
)doc");

}  // namespace tensorflow
