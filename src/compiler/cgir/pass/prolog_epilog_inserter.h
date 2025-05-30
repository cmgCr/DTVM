/*
 * Copyright (C) 2021-2023 the DTVM authors.
 */
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once

#include "compiler/cgir/cg_function.h"

namespace COMPILER {

class PrologEpilogInserter : public NonCopyable {
public:
  PrologEpilogInserter();
  ~PrologEpilogInserter();

  void runOnCgFunction(CgFunction &cg_func);

private:
  class Impl;
  std::unique_ptr<Impl> _impl;
};

} // namespace COMPILER
