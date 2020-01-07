/*
 *  This file is part of the MR utility library.
 *
 *  This code is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This code is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this code; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/* Copyright (C) 2019 Max-Planck-Society
   Author: Martin Reinecke */

#include "mr_util/error_handling.h"

using namespace std;

namespace mr {

namespace detail_error_handling {

bool abort_in_progress__ = false;

ostream &CodeLocation::print(ostream &os) const
  {
  os << file <<  ": " <<  line;
  if (func) os << " (" << func << ")";
  os << ":\n";
  return os;
  }

void killjob__()
  {
  // perhaps print stack trace?
  exit(1);
  }

}}