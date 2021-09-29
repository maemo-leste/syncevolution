/*
 *  File:         platform_time.cpp
 *
 *  Platform specific time implementations
 *
 *  Author:			  Lukas Zeller (luz@plan44.ch)
 *
 *  Copyright (c) 2002-2011 by Synthesis AG + plan44.ch
 *
 *  2004-11-15 : luz : extracted from lineartime.h
 */

// For tm_gmtoff on Linux.
#ifndef _STANDARD_SOURCE
# define _STANDARD_SOURCE 1
#endif
#include <time.h>

#include "prefix_file.h"

#include "lineartime.h"
#include "timezones.h"

// Linux uses standard c time stuff
//#warning "probably better use <cmath> here"
#ifndef ANDROID
#include <climits>
#include <cstdarg>
#include <cstdlib>
#endif

#include <cstdio>
#include <cstring>
// - Linux specific time stuff
#include <sys/time.h>
// - we need some library extras
#define __USE_MISC 1
#ifndef ANDROID
#include <ctime>
#endif

namespace sysync {



/// @brief get system real time
/// @return system's real time in lineartime_t scale, in specified time zone context
/// @param[in] aTimeContext desired output time zone
lineartime_t getSystemNowAs(timecontext_t aTimeContext,GZones *aGZones, bool noOffset)
{
  #ifdef NOW_WITH_MILLISECONDS
  // high precision time, UTC based
  struct timeval tv;
  // gettimeofday return seconds and milliseconds since start of the UNIX epoch
  gettimeofday(&tv,NULL);
  lineartime_t systime =
    (tv.tv_sec*secondToLinearTimeFactor+UnixToLineartimeOffset) +
    (tv.tv_usec*secondToLinearTimeFactor/1000000);
  #else
  // standard precision time (unix time), base is UTC
  time_t t = time(NULL);
  lineartime_t systime =
    t*secondToLinearTimeFactor+UnixToLineartimeOffset;
  #endif
  // - return as-is if requested time zone is UTC
  if (noOffset || TCTX_IS_UTC(aTimeContext))
    return systime; // return as-is
#ifndef ANDROID
  // - shortcut: conversion to system time is faster when done using
  //   glibc. tm_gmtoff is a glibc extension, so don't try to use it on
  //   Android.
  if (TCTX_IS_SYSTEM(aTimeContext)) {
    struct tm tm;
    if (localtime_r(
#ifdef NOW_WITH_MILLISECONDS
                    &tv.tv_sec
#else
                    &t
#endif
                    , &tm)) {
      return systime + (tm.tm_gmtoff * secondToLinearTimeFactor);
    }
  }
#endif
  // - convert to requested zone
  sInt32 aOffsSeconds;
  if (!TzOffsetSeconds(systime,TCTX_UTC,aTimeContext,aOffsSeconds,aGZones))
    return noLinearTime; // no time
  // return time with offset
  return systime + (aOffsSeconds * secondToLinearTimeFactor);
} // getSystemNowAs


/// @brief fine resolution sleep support
/// @param[in] aHowLong desired time to wait in lineartime_t units
void sleepLineartime(lineartime_t aHowLong)
{
  // Linux has nanosleep in nanoseconds
  timespec sleeptime;
  sleeptime.tv_sec=aHowLong/secondToLinearTimeFactor;
  sleeptime.tv_nsec=(aHowLong % secondToLinearTimeFactor)*1000000L;
  nanosleep(&sleeptime,NULL);
} // sleepLineartime




} // namespace sysync

/* eof */
