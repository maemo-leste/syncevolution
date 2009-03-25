/*
 * Copyright (C) 2008-2009 Patrick Ohly <patrick.ohly@gmx.de>
 */

#ifndef INCL_EVOLUTION_FILE_CONFIG_TREE
# define INCL_EVOLUTION_FILE_CONFIG_TREE

#include <ConfigTree.h>

#include <string>
#include <map>
using namespace std;

/**
 * This implementation maps nodes to plain .ini style files below an
 * absolute directory of the filesystem. The caller is responsible for
 * choosing that directory and how hidden and user-visible files are
 * to be named.
 */
class FileConfigTree : public ConfigTree {
 public:
    /**
     * @param root              absolute filesystem path
     * @param oldLayout         use file names as in SyncEvolution <= 0.7
     */
    FileConfigTree(const string &root,
                   bool oldLayout);

    /* ConfigTree API */
    virtual string getRootPath() const;
    virtual void flush();
    virtual void reset();
    virtual boost::shared_ptr<ConfigNode> open(const string &path,
                                               PropertyType type,
                                               const string &otherId = string(""));
    list<string> getChildren(const string &path);

 private:
    const string m_root;
    const bool m_oldLayout;

    typedef map< string, boost::shared_ptr<ConfigNode> > NodeCache_t;
    /** cache of all nodes ever accessed */
    NodeCache_t m_nodes;
};

#endif
