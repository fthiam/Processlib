//###########################################################################
// This file is part of ProcessLib, a submodule of LImA project the
// Library for Image Acquisition
//
// Copyright (C) : 2009-2011
// European Synchrotron Radiation Facility
// BP 220, Grenoble 38043
// FRANCE
//
// This is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
//
// This software is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//###########################################################################
#ifndef __unix
#pragma warning(disable:4251)
#endif

#include <pthread.h>
#include <map>
#include <string>

#include "processlib/Compatibility.h"

/** @brief this class manage error message in thread safe maner
 */
class DLL_EXPORT GslErrorMgr
{
  typedef std::map<pthread_t,std::string> ErrorMessageType;
  typedef std::map<pthread_t,int>	  ErrnoType;
 public:
  static inline GslErrorMgr& get() throw() {return GslErrorMgr::_errorMgr;}
  const char* lastErrorMsg() const;
  int   lastErrno() const;
  void	resetErrorMsg();
 private:
  ErrorMessageType	_errorMessage;
  ErrnoType		_lastGslErrno;
  static GslErrorMgr	_errorMgr;

  GslErrorMgr(); 
  static void _error_handler(const char*,const char *,int,int);
};
