//===- DialectRegistration.cpp - MLIR GPU dialect registration ------------===//
//
// Part of the MLIR Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/GPU/GPUDialect.h"

// Static initialization for GPU dialect registration.
static mlir::DialectRegistration<mlir::gpu::GPUDialect> kernelDialect;
