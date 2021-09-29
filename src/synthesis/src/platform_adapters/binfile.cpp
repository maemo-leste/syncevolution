/**
 *  @File     binfile.cpp
 *
 *  @Author   Lukas Zeller (luz@plan44.ch)
 *
 *  @brief TBinFile
 *    Standard C file implementation of TBinFile
 *
 *    Copyright (c) 2003-2011 by Synthesis AG + plan44.ch
 *
 *  @Date 2006-03-28 : luz : created
 */
/*
 */

#include "prefix_file.h"
#include "binfile.h"

#ifdef WINCE
#include "win32_utils.h"
#endif

#include <string.h>
#include <vector>

namespace sysync {


// @brief constructor
TBinFile::TBinFile() :
  fCBinFile(NULL),
  fPos(0)
{
} // TBinFile


// @brief destructor
TBinFile::~TBinFile()
{
  destruct();
} // TBinFile::~TBinFile


// @brief test if platform file open
bool TBinFile::platformFileIsOpen(void)
{
  return fCBinFile!=NULL;
} // TBinFile::platformFileIsOpen

// A special FILE * pointer never returned by fopen().
#define TBINFILE_MEM_FILE stdin
#define TBINFILE_MEM_FILE_PERFIX "/dev/null/"

static bool IsNullFile(cAppCharP aFilePath)
{
  return !strncmp(aFilePath, TBINFILE_MEM_FILE_PERFIX, strlen(TBINFILE_MEM_FILE_PERFIX));
}


/// @brief Open a platform file
bool TBinFile::platformOpenFile(cAppCharP aFilePath, TFileOpenModes aMode)
{
  if (IsNullFile(aFilePath)) {
    fCBinFile = TBINFILE_MEM_FILE;
    fPos = 0;
    return true;
  }

  switch (aMode) {
    case fopm_update :
      fCBinFile = fopen(aFilePath,"r+b");
      break;
    case fopm_create :
      fCBinFile = fopen(aFilePath,"w+b");
      break;
  }
  return fCBinFile!=NULL;
} // TBinFile::platformOpenFile


/// @brief close file
bool TBinFile::platformCloseFile(void)
{
  if (fCBinFile && fCBinFile != TBINFILE_MEM_FILE)
    fclose(fCBinFile);
  fCBinFile=NULL;
  return true;
} // TBinFile::platformCloseFile


/// @brief seek in file
bool TBinFile::platformSeekFile(uInt32 aPos, bool aFromEnd)
{
  if (fCBinFile == TBINFILE_MEM_FILE) {
    if (aFromEnd) {
      if (aPos > fContent.size()) {
        return false;
      }
      fPos = fContent.size() - aPos;
    } else {
      fPos = aPos;
    }
    return true;
  }

  return fseek(fCBinFile,aPos,aFromEnd ? SEEK_END : SEEK_SET) == 0;
} // TBinFile::platformSeekFile


/// @brief read from file
bool TBinFile::platformReadFile(void *aBuffer, uInt32 aMaxRead)
{
  if (fCBinFile == TBINFILE_MEM_FILE) {
    if (fPos + aMaxRead <= fContent.size()) {
      memcpy(aBuffer, fContent.c_str() + fPos, aMaxRead);
      return true;
    } else {
      return false;
    }
  }

  return fread(aBuffer,aMaxRead,1,fCBinFile) == 1;
} // TBinFile::platformReadFile


/// @brief write to file
bool TBinFile::platformWriteFile(const void *aBuffer, uInt32 aBytes)
{
  if (fCBinFile == TBINFILE_MEM_FILE) {
    if (fPos < fContent.size()) {
      size_t partial = fContent.size() - fPos;
      if (partial > aBytes) {
        partial = aBytes;
      }
      fContent.replace(fPos, partial, (const char *)aBuffer, partial);
      fContent.append((const char *)aBuffer + partial, aBytes - partial);
    } else {
      fContent.resize(fPos);
      fContent.append((const char *)aBuffer, aBytes);
    }
    fPos += aBytes;
    return true;
  }

  // Don't overwrite content which is already exactly what we want it
  // to be.
  std::vector<char> buffer(aBytes);
  char *addr = &buffer.front();
  long offset = ftell(fCBinFile);
  if (offset == -1)
    return false;
  if (fread(addr,aBytes,1,fCBinFile) != 1 ||
      memcmp(addr,aBuffer,aBytes)) {
    return fseek(fCBinFile,offset,SEEK_SET) == 0 &&
      fwrite(aBuffer,aBytes,1,fCBinFile) == 1;
  } else {
    return true;
  }
} // TBinFile::platformWriteFile


/// @brief flush all buffers
bool TBinFile::platformFlushFile(void)
{
  if (fCBinFile == TBINFILE_MEM_FILE) {
    return true;
  }

  return fflush(fCBinFile)==0;
} // TBinFile::platformFlushFile


/// @brief truncate file to a specific length
bool TBinFile::platformTruncateFile(uInt32 aNewSize)
{
  if (fCBinFile == TBINFILE_MEM_FILE) {
    fContent.resize(aNewSize);
    return true;
  }

  #if defined(LINUX) || defined(MACOSX)
    fflush(fCBinFile); // unbuffer everything
    int fd = fileno(fCBinFile); // get file descriptor
    struct stat buffer;
    if (fstat(fd, &buffer) ||
        buffer.st_size == (off_t)aNewSize || // avoid modifying file if it already has the right size
        ftruncate(fd,aNewSize)) {
      ; // error ignored
    }
  #elif defined(WIN32)
    fflush(fCBinFile); // unbuffer everything
    HANDLE h = (HANDLE)fileno(fCBinFile); // get file descriptor
    SetFilePointer(h,aNewSize,NULL,FILE_BEGIN);
    SetEndOfFile(h);
  #elif defined(__EPOC_OS__)
    #ifdef RELEASE_VERSION
      #warning "%%% we need file truncation for symbian here"
    #endif
    return false;
  #else
    #error "file truncation not implemented for this platform"
  #endif
  // successfully truncated
  return true;
} // TBinFile::platformTruncateFile


/// @delete file entirely
bool TBinFile::platformDeleteFile(cAppCharP aFilePath)
{
  if (IsNullFile(aFilePath)) {
    fPos = 0;
    fContent.clear();
    return true;
  }

  #if defined(LINUX) || defined(MACOSX)
    unlink(aFilePath);
  #elif defined(WINCE)
    TCHAR filename[MAX_PATH];
    UTF8toWCharBuf(aFilePath,filename,MAX_PATH);
    DeleteFileW(filename);
  #elif defined(WIN32)
    unlink(aFilePath);
  #else
    #error "file delete not implemented for this platform"
  #endif
  // successfully deleted
  return true;
} // TBinFile::platformDeleteFile



} // namespace sysync

// eof
