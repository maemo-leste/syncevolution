/*
 *  File:         sysync_noncopyable.h
 *
 *  Author:       Patrick Ohly (patrick.ohly@intel.com)
 *
 *  Derive from sysync::noncopyable in all classes which must not be copied,
 *  for example because they contain pointers.
 *  Accidentally copying with the default copy constructor will then
 *  lead to compile instead of runtime errors.
 *
 *  Same approach as in boost::noncopyable.
 *
 *  Copyright (c) 2013 by Synthesis AG + plan44.ch
 */

#ifndef SYSYNC_NONCOPYABLE_H
#define SYSYNC_NONCOPYABLE_H

namespace sysync {

class noncopyable
{
 protected:
  noncopyable() {}
  ~noncopyable() {}
 private:
  noncopyable( const noncopyable& );
  const noncopyable& operator=( const noncopyable& );
};

} // namespace sysync

#endif // SYSYNC_NONCOPYABLE
