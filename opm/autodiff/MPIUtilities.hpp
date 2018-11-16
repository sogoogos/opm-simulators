/*
  Copyright 2015 SINTEF ICT, Applied Mathematics.

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef OPM_MPIUTILITIES_HEADER_INCLUDED
#define OPM_MPIUTILITIES_HEADER_INCLUDED

#include <boost/any.hpp>

namespace Opm
{
    /// Return true if this is a serial run, or rank zero on an MPI run.
    bool isIORank(const boost::any& parallel_info);

} // namespace Opm

#endif // OPM_MPIUTILITIES_HEADER_INCLUDED