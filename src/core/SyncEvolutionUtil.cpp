/*
 * Copyright (C) 2008 Patrick Ohly
 */

#include "config.h"
#include "SyncEvolutionUtil.h"
#include "EvolutionSyncClient.h"
#include "TransportAgent.h"
#include "Logging.h"

#include <synthesis/syerror.h>

#include <boost/scoped_array.hpp>
#include <boost/foreach.hpp>
#include <fstream>

#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#ifdef ENABLE_UNIT_TESTS
#include <cppunit/extensions/HelperMacros.h>
CPPUNIT_REGISTRY_ADD_TO_DEFAULT("SyncEvolution");
#endif

string normalizePath(const string &path)
{
    string res;

    res.reserve(path.size());
    size_t index = 0;
    while (index < path.size()) {
        char curr = path[index];
        res += curr;
        index++;
        if (curr == '/') {
            while (index < path.size() &&
                   (path[index] == '/' ||
                    (path[index] == '.' &&
                     index + 1 < path.size() &&
                     (path[index + 1] == '.' ||
                      path[index + 1] == '/')))) {
                index++;
            }
        }
    }
    if (!res.empty() && res[res.size() - 1] == '/') {
        res.resize(res.size() - 1);
    }
    return res;
}

void mkdir_p(const string &path)
{
    boost::scoped_array<char> dirs(new char[path.size() + 1]);
    char *curr = dirs.get();
    strcpy(curr, path.c_str());
    do {
        char *nextdir = strchr(curr, '/');
        if (nextdir) {
            *nextdir = 0;
            nextdir++;
        }
        if (*curr) {
            if (access(dirs.get(),
                       nextdir ? (R_OK|X_OK) : (R_OK|X_OK|W_OK)) &&
                (errno != ENOENT ||
                 mkdir(dirs.get(), 0777))) {
                EvolutionSyncClient::throwError(string(dirs.get()), errno);
            }
        }
        if (nextdir) {
            nextdir[-1] = '/';
        }
        curr = nextdir;
    } while (curr);
}

void rm_r(const string &path)
{
    struct stat buffer;
    if (lstat(path.c_str(), &buffer)) {
        if (errno == ENOENT) {
            return;
        } else {
            EvolutionSyncClient::throwError(path, errno);
        }
    }

    if (!S_ISDIR(buffer.st_mode)) {
        if (!unlink(path.c_str())) {
            return;
        } else {
            EvolutionSyncClient::throwError(path, errno);
        }
    }

    ReadDir dir(path);
    BOOST_FOREACH(const string &entry, dir) {
        rm_r(path + "/" + entry);
    }
    if (rmdir(path.c_str())) {
        EvolutionSyncClient::throwError(path, errno);
    }
}

bool isDir(const string &path)
{
    DIR *dir = opendir(path.c_str());
    if (dir) {
        closedir(dir);
        return true;
    } else if (errno != ENOTDIR && errno != ENOENT) {
        EvolutionSyncClient::throwError(path, errno);
    }

    return false;
}

UUID::UUID()
{
    static class InitSRand {
    public:
        InitSRand() {
            ifstream seedsource("/dev/urandom");
            unsigned int seed;
            if (!seedsource.get((char *)&seed, sizeof(seed))) {
                seed = time(NULL);
            }
            srand(seed);
        }
    } initSRand;

    char buffer[16 * 4 + 5];
    sprintf(buffer, "%08x-%04x-%04x-%02x%02x-%08x%04x",
            rand() & 0xFFFFFFFF,
            rand() & 0xFFFF,
            (rand() & 0x0FFF) | 0x4000 /* RFC 4122 time_hi_and_version */,
            (rand() & 0xBF) | 0x80 /* clock_seq_hi_and_reserved */,
            rand() & 0xFF,
            rand() & 0xFFFFFFFF,
            rand() & 0xFFFF
            );
    this->assign(buffer);
}


ReadDir::ReadDir(const string &path) : m_path(path)
{
    DIR *dir = NULL;

    try {
        dir = opendir(path.c_str());
        if (!dir) {
            EvolutionSyncClient::throwError(path, errno);
        }
        errno = 0;
        struct dirent *entry = readdir(dir);
        while (entry) {
            if (strcmp(entry->d_name, ".") &&
                strcmp(entry->d_name, "..")) {
                m_entries.push_back(entry->d_name);
            }
            entry = readdir(dir);
        }
        if (errno) {
            EvolutionSyncClient::throwError(path, errno);
        }
    } catch(...) {
        if (dir) {
            closedir(dir);
        }
        throw;
    }

    closedir(dir);
}

bool ReadFile(const string &filename, string &content)
{
    ifstream in;
    in.open(filename.c_str());
    ostringstream out;
    char buf[8192];
    do {
        in.read(buf, sizeof(buf));
        out.write(buf, in.gcount());
    } while(in);

    content = out.str();
    return in.eof();
}

unsigned long Hash(const char *str)
{
    unsigned long hashval = 5381;
    int c;

    while ((c = *str++) != 0) {
        hashval = ((hashval << 5) + hashval) + c;
    }

    return hashval;
}

std::string StringPrintf(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    std::string res = StringPrintfV(format, ap);
    va_end(ap);
    return res;
}

std::string StringPrintfV(const char *format, va_list ap)
{
    va_list aq;

    char *buffer = NULL;
    ssize_t size = 0;
    ssize_t realsize = 255;
    do {
        // vsnprintf() destroys ap, so make a copy first
        va_copy(aq, ap);

        if (size < realsize) {
            buffer = (char *)realloc(buffer, realsize + 1);
            if (!buffer) {
                if (buffer) {
                    free(buffer);
                }
                return "";
            }
            size = realsize;
        }

        realsize = vsnprintf(buffer, size + 1, format, aq);
        if (realsize == -1) {
            // old-style vnsprintf: exact len unknown, try again with doubled size
            realsize = size * 2;
        }
        va_end(aq);
    } while(realsize > size);

    std::string res = buffer;
    free(buffer);
    return res;
}

SyncMLStatus SyncEvolutionException::handle(SyncMLStatus *status)
{
    SyncMLStatus new_status = STATUS_FATAL;

    try {
        throw;
    } catch (const TransportException &ex) {
        SE_LOG_DEBUG(NULL, NULL, "TransportException thrown at %s:%d",
                     ex.m_file.c_str(), ex.m_line);
        SE_LOG_ERROR(NULL, NULL, "%s", ex.what());
        new_status = SyncMLStatus(sysync::LOCERR_TRANSPFAIL);
    } catch (const SyncEvolutionException &ex) {
        SE_LOG_DEBUG(NULL, NULL, "exception thrown at %s:%d",
                     ex.m_file.c_str(), ex.m_line);
        SE_LOG_ERROR(NULL, NULL, "%s", ex.what());
    } catch (const std::exception &ex) {
        SE_LOG_ERROR(NULL, NULL, "%s", ex.what());
    } catch (...) {
        SE_LOG_ERROR(NULL, NULL, "unknown error");
    }

    if (status && *status == STATUS_OK) {
        *status = new_status;
    }
    return status ? *status : new_status;
}
