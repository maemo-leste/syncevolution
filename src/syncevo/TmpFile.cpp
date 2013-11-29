/*
 * Copyright (C) 2012 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) version 3.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */


#include <cstdio>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "TmpFile.h"


TmpFile::TmpFile() :
    m_fd(-1),
    m_mapptr(0),
    m_mapsize(0)
{
}


TmpFile::~TmpFile()
{
    try {
        unmap();
        close();
    } catch (std::exception &x) {
        fprintf(stderr, "TmpFile::~TmpFile(): %s\n", x.what());
    } catch (...) {
        fputs("TmpFile::~TmpFile(): unknown exception\n", stderr);
    }
}


void TmpFile::create()
{
    gchar *filename = NULL;
    GError *error = NULL;

    if (m_fd >= 0 || m_mapptr || m_mapsize) {
        throw TmpFileException("TmpFile::create(): busy");
    }
    m_fd = g_file_open_tmp(NULL, &filename, &error);
    if (error != NULL) {
        throw TmpFileException(
            std::string("TmpFile::create(): g_file_open_tmp(): ") +
            std::string(error->message));
    }
    m_filename = filename;
    g_free(filename);
}


void TmpFile::map(void **mapptr, size_t *mapsize)
{
    struct stat sb;

    if (m_mapptr || m_mapsize) {
        throw TmpFileException("TmpFile::map(): busy");
    }
    if (m_fd < 0) {
        throw TmpFileException("TmpFile::map(): m_fd < 0");
    }
    if (fstat(m_fd, &sb) != 0) {
        throw TmpFileException("TmpFile::map(): fstat()");
    }
    m_mapptr = mmap(NULL, sb.st_size, PROT_READ|PROT_WRITE, MAP_PRIVATE,
                    m_fd, 0);
    if (m_mapptr == MAP_FAILED) {
        m_mapptr = 0;
        throw TmpFileException("TmpFile::map(): mmap()");
    }
    m_mapsize = sb.st_size;

    if (mapptr != NULL) {
        *mapptr = m_mapptr;
    }
    if (mapsize != NULL) {
        *mapsize = m_mapsize;
    }
}


void TmpFile::unmap()
{
    if (m_mapptr && m_mapsize) {
        munmap(m_mapptr, m_mapsize);
    }
    m_mapsize = 0;
    m_mapptr = 0;
}

size_t TmpFile::moreData() const
{
    if (m_fd >= 0) {
        struct stat sb;
        if (fstat(m_fd, &sb) != 0) {
            throw TmpFileException("TmpFile::map(): fstat()");
        }
        if ((!m_mapptr && sb.st_size) ||
            (sb.st_size > 0 && m_mapsize < (size_t)sb.st_size)) {
            return sb.st_size - m_mapsize;
        }
    }

    return 0;
}


void TmpFile::remove()
{
    if (!m_filename.empty()) {
        unlink(m_filename.c_str());
        m_filename.clear();
    }
}

void TmpFile::close()
{
    remove();
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}


pcrecpp::StringPiece TmpFile::stringPiece()
{
    pcrecpp::StringPiece sp;

    if (!(m_mapptr && m_mapsize)) {
        map();
    }
    sp.set(m_mapptr, static_cast<int> (m_mapsize));
    return sp;
}
