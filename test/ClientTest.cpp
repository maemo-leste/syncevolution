/*
 * Copyright (C) 2008 Funambol, Inc.
 * Copyright (C) 2008-2009 Patrick Ohly <patrick.ohly@gmx.de>
 * Copyright (C) 2009 Intel Corporation
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

/** @cond API */
/** @addtogroup ClientTest */
/** @{ */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef ENABLE_INTEGRATION_TESTS

#include "ClientTest.h"
#include "test.h"
#include <SyncSource.h>
#include <TransportAgent.h>
#include <Logging.h>
#include <syncevo/util.h>
#include <syncevo/SyncContext.h>
#include <VolatileConfigNode.h>

#include <synthesis/dataconversion.h>

#include <memory>
#include <vector>
#include <set>
#include <utility>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <algorithm>

#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <boost/bind.hpp>
#include <boost/tokenizer.hpp>

#include <syncevo/declarations.h>

#ifdef ENABLE_BUTEO_TESTS
#include "client-test-buteo.h"
#endif

SE_BEGIN_CXX

static set<ClientTest::Cleanup_t> cleanupSet;

/**
 * true when running as server,
 * relevant for sources instantiated by us
 * and testConversion, which does not work in
 * server mode (Synthesis engine not in the right
 * state when we try to run the test)
 */
static bool isServerMode()
{
    const char *serverMode = getenv("CLIENT_TEST_MODE");
    return serverMode && !strcmp(serverMode, "server");
}

/**
 * CLIENT_TEST_SERVER env variable or "" if unset
 */
std::string currentServer()
{
    const char *tmp = getenv("CLIENT_TEST_SERVER");
    return tmp ? tmp : "";
}

/**
 * CLIENT_TEST_NUM_ITEMS env variable or 100
 */
int defNumItems()
{
    char *numitems = getenv("CLIENT_TEST_NUM_ITEMS");
    return numitems ? atoi(numitems) : 100;
}

static SyncMode RefreshFromPeerMode()
{
    return isServerMode() ? SYNC_REFRESH_FROM_CLIENT : SYNC_REFRESH_FROM_SERVER;
}

static SyncMode RefreshFromLocalMode()
{
    return isServerMode() ? SYNC_REFRESH_FROM_SERVER : SYNC_REFRESH_FROM_CLIENT;
}

static SyncMode OneWayFromPeerMode()
{
    return isServerMode() ? SYNC_ONE_WAY_FROM_CLIENT : SYNC_ONE_WAY_FROM_SERVER;
}

static SyncMode OneWayFromLocalMode()
{
    return isServerMode() ? SYNC_ONE_WAY_FROM_SERVER : SYNC_ONE_WAY_FROM_CLIENT;
}

/**
 * Using this pointer automates the open()/beginSync()/endSync()/close()
 * life cycle: it automatically calls these functions when a new
 * pointer is assigned or deleted.
 *
 * Anchors are stored globally in a hash which uses the tracking node
 * name as key. This name happens to be the unique file path that
 * is created for each source (see TestEvolution::createSource() and
 * SyncConfig::getSyncSourceNodes()).
 */
class TestingSyncSourcePtr : public std::auto_ptr<TestingSyncSource>
{
    typedef std::auto_ptr<TestingSyncSource> base_t;

    static StringMap m_anchors;

public:
    TestingSyncSourcePtr() {}
    TestingSyncSourcePtr(TestingSyncSource *source) :
        base_t(source)
    {
        CPPUNIT_ASSERT(source);
        source->open();
        string node = source->getTrackingNode()->getName();
        source->beginSync(m_anchors[node], "");
        if (isServerMode()) {
            source->enableServerMode();
        }
    }
    ~TestingSyncSourcePtr()
    {
        reset(NULL);
    }

    void reset(TestingSyncSource *source = NULL)
    {
        if (this->get()) {
            BOOST_FOREACH(const SyncSource::Operations::CallbackFunctor_t &callback,
                          get()->getOperations().m_endSession) {
                callback();
            }
            string node = get()->getTrackingNode()->getName();
            m_anchors[node] = get()->endSync(true);
            get()->close();
        }
        base_t::reset(source);
        if (source) {
            source->open();
            string node = source->getTrackingNode()->getName();
            source->beginSync(m_anchors[node], "");
            if (isServerMode()) {
                source->enableServerMode();
            }
            BOOST_FOREACH(const SyncSource::Operations::CallbackFunctor_t &callback,
                          source->getOperations().m_endSession) {
                callback();
            }
        }
    }
};

StringMap TestingSyncSourcePtr::m_anchors;

bool SyncOptions::defaultWBXML()
{
    const char *t = getenv("CLIENT_TEST_XML");
    if (t && (!strcmp(t, "1") || !strcasecmp(t, "t"))) {
        // use XML
        return false;
    } else {
        return true;
    }
}

std::list<std::string> listItemsOfType(TestingSyncSource *source, int state)
{
    std::list<std::string> res;

    BOOST_FOREACH(const string &luid, source->getItems(SyncSourceChanges::State(state))) {
        res.push_back(luid);
    }
    return res;
}
static std::list<std::string> listNewItems(TestingSyncSource *source) { return listItemsOfType(source, SyncSourceChanges::NEW); }
static std::list<std::string> listUpdatedItems(TestingSyncSource *source) { return listItemsOfType(source, SyncSourceChanges::UPDATED); }
static std::list<std::string> listDeletedItems(TestingSyncSource *source) { return listItemsOfType(source, SyncSourceChanges::DELETED); }
static std::list<std::string> listItems(TestingSyncSource *source) { return listItemsOfType(source, SyncSourceChanges::ANY); }

int countItemsOfType(TestingSyncSource *source, int type) { return source->getItems(SyncSourceChanges::State(type)).size(); }
static int countNewItems(TestingSyncSource *source) { return countItemsOfType(source, SyncSourceChanges::NEW); }
static int countUpdatedItems(TestingSyncSource *source) { return countItemsOfType(source, SyncSourceChanges::UPDATED); }
static int countDeletedItems(TestingSyncSource *source) { return countItemsOfType(source, SyncSourceChanges::DELETED); }
static int countItems(TestingSyncSource *source) { return countItemsOfType(source, SyncSourceChanges::ANY); }


/** insert new item, return LUID */
static std::string importItem(TestingSyncSource *source, const ClientTestConfig &config, std::string &data)
{
    CPPUNIT_ASSERT(source);
    if (data.size()) {
        SyncSourceRaw::InsertItemResult res;
        SOURCE_ASSERT_NO_FAILURE(source, res = source->insertItemRaw("", config.m_mangleItem(data, false)));
        CPPUNIT_ASSERT(!res.m_luid.empty());
        return res.m_luid;
    } else {
        return "";
    }
}

static void restoreStorage(const ClientTest::Config &config, ClientTest &client)
{
#ifdef ENABLE_BUTEO_TESTS
    if (boost::iequals(config.sourceName,"qt_contact")) { 
        QtContactsSwitcher::restoreStorage(client); 
    }
#endif
}

static void backupStorage(const ClientTest::Config &config, ClientTest &client)
{
#ifdef ENABLE_BUTEO_TESTS
    if (boost::iequals(config.sourceName,"qt_contact")) { 
        QtContactsSwitcher::backupStorage(client); 
    }
#endif
}

/** adds the supported tests to the instance itself */
void LocalTests::addTests() {
    if (config.m_createSourceA) {
        ADD_TEST(LocalTests, testOpen);
        ADD_TEST(LocalTests, testIterateTwice);
        if (!config.m_insertItem.empty()) {
            ADD_TEST(LocalTests, testSimpleInsert);
            ADD_TEST(LocalTests, testLocalDeleteAll);
            ADD_TEST(LocalTests, testComplexInsert);

            if (!config.m_updateItem.empty()) {
                ADD_TEST(LocalTests, testLocalUpdate);

                if (config.m_createSourceB) {
                    ADD_TEST(LocalTests, testChanges);
                }
            }

            if (config.m_import &&
                config.m_dump &&
                config.m_compare &&
                !config.m_testcases.empty()) {
                ADD_TEST(LocalTests, testImport);
                ADD_TEST(LocalTests, testImportDelete);
            }

            if (!config.m_templateItem.empty() &&
                !config.m_uniqueProperties.empty()) {
                ADD_TEST(LocalTests, testManyChanges);
            }

            // create a sub-suite for each set of linked items
            for (int i = 0; i < (int)config.m_linkedItems.size(); i++) {
                CppUnit::TestSuite *linked = new CppUnit::TestSuite(getName() + StringPrintf("::LinkedItems_%d", i));
                ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsParent);
                if (config.m_linkedItemsRelaxedSemantic) {
                    ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsChild);
                }
                ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsParentChild);
                if (config.m_linkedItemsRelaxedSemantic) {
                    ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsChildParent);
                }
                if (config.m_linkedItemsRelaxedSemantic) {
                    ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsChildChangesParent);
                }
                if (config.m_linkedItemsRelaxedSemantic) {
                    ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsRemoveParentFirst);
                }
                ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsRemoveNormal);
                if (config.m_sourceKnowsItemSemantic) {
                    ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsInsertParentTwice);
                    if (config.m_linkedItemsRelaxedSemantic) {
                        ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsInsertChildTwice);
                    }
                }
                ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsParentUpdate);
                if (config.m_linkedItemsRelaxedSemantic) {
                    ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsUpdateChild);
                }
                ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsInsertBothUpdateChild);
                ADD_TEST_TO_SUITE(linked, LocalTests, testLinkedItemsInsertBothUpdateParent);

                addTest(linked);
            }
        }
    }
}

std::string LocalTests::insert(CreateSource createSource, const std::string &data, bool relaxed, std::string *inserted) {
    restoreStorage(config, client);

    // create source
    TestingSyncSourcePtr source(createSource());

    // count number of already existing items
    int numItems = 0;
    CPPUNIT_ASSERT_NO_THROW(numItems = countItems(source.get()));
    SyncSourceRaw::InsertItemResult res;
    std::string mangled = config.m_mangleItem(data, false);
    if (inserted) {
        *inserted = mangled;
    }
    SOURCE_ASSERT_NO_FAILURE(source.get(), res = source->insertItemRaw("", mangled));
    CPPUNIT_ASSERT(!res.m_luid.empty());

    // delete source again
    CPPUNIT_ASSERT_NO_THROW(source.reset());

    if (!relaxed) {
        // two possible results:
        // - a new item was added
        // - the item was matched against an existing one
        CPPUNIT_ASSERT_NO_THROW(source.reset(createSource()));
        CPPUNIT_ASSERT_EQUAL(numItems + (res.m_merged ? 0 : 1),
                             countItems(source.get()));
        CPPUNIT_ASSERT(countNewItems(source.get()) == 0);
        CPPUNIT_ASSERT(countUpdatedItems(source.get()) == 0);
        CPPUNIT_ASSERT(countDeletedItems(source.get()) == 0);
    }
    backupStorage(config, client);

    return res.m_luid;
}

/** deletes specific item locally via sync source */
static std::string updateItem(CreateSource createSource, const ClientTestConfig &config, const std::string &uid, const std::string &data, std::string *updated = NULL) {
    std::string newuid;

    CPPUNIT_ASSERT(createSource.createSource);

    // create source
    TestingSyncSourcePtr source(createSource());

    // insert item
    SyncSourceRaw::InsertItemResult res;
    std::string mangled = config.m_mangleItem(data, true);
    if (updated) {
        *updated = mangled;
    }
    SOURCE_ASSERT_NO_FAILURE(source.get(), res = source->insertItemRaw(uid, mangled.c_str()));
    SOURCE_ASSERT(source.get(), !res.m_luid.empty());

    return res.m_luid;
}

/** updates specific item locally via sync source */
static void removeItem(CreateSource createSource, const std::string &luid)
{
    CPPUNIT_ASSERT(createSource.createSource);

    // create source
    TestingSyncSourcePtr source(createSource());

    // remove item
    SOURCE_ASSERT_NO_FAILURE(source.get(), source->deleteItem(luid));
}

void LocalTests::update(CreateSource createSource, const std::string &data, bool check) {
    CPPUNIT_ASSERT(createSource.createSource);

    restoreStorage(config, client);

    // create source
    TestingSyncSourcePtr source(createSource());

    // get existing item, then update it
    SyncSourceChanges::Items_t::const_iterator it;
    SOURCE_ASSERT_NO_FAILURE(source.get(), it = source->getAllItems().begin());
    CPPUNIT_ASSERT(it != source->getAllItems().end());
    string luid = *it;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source->insertItemRaw(luid, config.m_mangleItem(data, true)));
    CPPUNIT_ASSERT_NO_THROW(source.reset());

    if (!check) {
        return;
    }

    // check that the right changes are reported when reopening the source
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSource()));
    CPPUNIT_ASSERT_EQUAL(1, countItems(source.get()));
    CPPUNIT_ASSERT_EQUAL(0, countNewItems(source.get()));
    CPPUNIT_ASSERT_EQUAL(0, countUpdatedItems(source.get()));
    CPPUNIT_ASSERT_EQUAL(0, countDeletedItems(source.get()));
    
    SOURCE_ASSERT_NO_FAILURE(source.get(), it = source->getAllItems().begin());
    CPPUNIT_ASSERT(it != source->getAllItems().end());
    CPPUNIT_ASSERT_EQUAL(luid, *it);

    backupStorage(config, client);
}

void LocalTests::update(CreateSource createSource, const std::string &data, const std::string &luid) {
    CPPUNIT_ASSERT(createSource.createSource);

    restoreStorage(config, client);
    // create source
    TestingSyncSourcePtr source(createSource());

    // update it
    SOURCE_ASSERT_NO_FAILURE(source.get(), source->insertItemRaw(luid, config.m_mangleItem(data, true)));

    backupStorage(config, client);
}

/** deletes all items locally via sync source */
void LocalTests::deleteAll(CreateSource createSource) {
    CPPUNIT_ASSERT(createSource.createSource);

    restoreStorage(config, client);
    // create source
    TestingSyncSourcePtr source(createSource());

    // delete all items
    SOURCE_ASSERT_NO_FAILURE(source.get(), source->removeAllItems());
    CPPUNIT_ASSERT_NO_THROW(source.reset());

    // check that all items are gone
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSource()));
    SOURCE_ASSERT_MESSAGE(
        "should be empty now",
        source.get(),
        countItems(source.get()) == 0);
    CPPUNIT_ASSERT_EQUAL( 0, countNewItems(source.get()) );
    CPPUNIT_ASSERT_EQUAL( 0, countUpdatedItems(source.get()) );
    CPPUNIT_ASSERT_EQUAL( 0, countDeletedItems(source.get()) );
    backupStorage(config, client);
}

/** deletes specific item locally via sync source */
static void deleteItem(CreateSource createSource, const std::string &uid) {
    CPPUNIT_ASSERT(createSource.createSource);

    // create source
    TestingSyncSourcePtr source(createSource());

    // delete item
    SOURCE_ASSERT_NO_FAILURE(source.get(), source->deleteItem(uid));
}

/**
 * takes two databases, exports them,
 * then compares them using synccompare
 *
 * @param refFile      existing file with source reference items, NULL uses a dump of sync source A instead
 * @param copy         a sync source which contains the copied items, begin/endSync will be called
 * @param raiseAssert  raise assertion if comparison yields differences (defaults to true)
 */
bool LocalTests::compareDatabases(const char *refFile, TestingSyncSource &copy, bool raiseAssert) {
    CPPUNIT_ASSERT(config.m_dump);

    std::string sourceFile, copyFile;

    if (refFile) {
        sourceFile = refFile;
    } else {
        sourceFile = getCurrentTest() + ".A.test.dat";
        simplifyFilename(sourceFile);
        TestingSyncSourcePtr source;
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceA()));
        SOURCE_ASSERT_EQUAL(source.get(), 0, config.m_dump(client, *source.get(), sourceFile));
        CPPUNIT_ASSERT_NO_THROW(source.reset());
    }

    copyFile = getCurrentTest() + ".B.test.dat";
    simplifyFilename(copyFile);
    SOURCE_ASSERT_EQUAL(&copy, 0, config.m_dump(client, copy, copyFile));

    bool equal = config.m_compare(client, sourceFile, copyFile);
    CPPUNIT_ASSERT(!raiseAssert || equal);

    return equal;
}

/**
 * compare data in source with vararg list of std::string pointers, NULL terminated
 */
void LocalTests::compareDatabases(TestingSyncSource &copy,
                                  ...)
{
    std::string sourceFile = getCurrentTest() + ".ref.test.dat";
    ofstream out(sourceFile.c_str());
    va_list ap;
    va_start(ap, copy);
    std::string *item;
    while ((item = va_arg(ap, std::string *)) != NULL) {
        out << *item;
    }
    va_end(ap);
    out.close();
    compareDatabases(sourceFile.c_str(), copy);
}


std::string LocalTests::createItem(int item, const std::string &revision, int size)
{
    std::string data = config.m_mangleItem(config.m_templateItem, false);
    std::stringstream prefix;

    // string to be inserted at start of unique properties;
    // avoid adding white space (not sure whether it is valid for UID)
    prefix << std::setfill('0') << std::setw(3) << item << "-";

    BOOST_FOREACH (std::string curProp,
                   boost::tokenizer< boost::char_separator<char> >(config.m_uniqueProperties,
                                                                   boost::char_separator<char>(":"))) {
        std::string property;
        // property is expected to not start directly at the
        // beginning
        property = "\n";
        property += curProp;
        property += ":";
        size_t off = data.find(property);
        if (off != data.npos) {
            data.insert(off + property.size(), prefix.str());
        }
    }
    boost::replace_all(data, "<<UNIQUE>>", prefix.str());
    boost::replace_all(data, "<<REVISION>>", revision);
    if (size > 0 && (int)data.size() < size) {
        int additionalBytes = size - (int)data.size();
        int added = 0;
        /* vCard 2.1 and vCal 1.0 need quoted-printable line breaks */
        bool quoted = data.find("VERSION:1.0") != data.npos ||
            data.find("VERSION:2.1") != data.npos;
        size_t toreplace = 1;

        CPPUNIT_ASSERT(!config.m_sizeProperty.empty());

        /* stuff the item so that it reaches at least that size */
        size_t off = data.find(config.m_sizeProperty);
        CPPUNIT_ASSERT(off != data.npos);
        std::stringstream stuffing;
        if (quoted) {
            stuffing << ";ENCODING=QUOTED-PRINTABLE:";
        } else {
            stuffing << ":";
        }

        // insert after the first line, it often acts as the summary
        if (data.find("BEGIN:VJOURNAL") != data.npos) {
            size_t start = data.find(":", off);
            CPPUNIT_ASSERT( start != data.npos );
            size_t eol = data.find("\\n", off);
            CPPUNIT_ASSERT( eol != data.npos );
            stuffing << data.substr(start + 1, eol - start + 1);
            toreplace += eol - start + 1;
        }

        while(added < additionalBytes) {
            int linelen = 0;

            while(added + 4 < additionalBytes &&
                  linelen < 60) {
                stuffing << 'x';
                added++;
                linelen++;
            }
            // insert line breaks to allow folding
            if (quoted) {
                stuffing << "x=0D=0Ax";
                added += 8;
            } else {
                stuffing << "x\\nx";
                added += 4;
            }
        }
        off = data.find(":", off);
        data.replace(off, toreplace, stuffing.str());
    }

    return data;
}


/**
 * insert artificial items, number of them 100
 * unless passed explicitly
 *
 * @param createSource    a factory for the sync source that is to be used
 * @param startIndex      IDs are generated starting with this value
 * @param numItems        number of items to be inserted if non-null, otherwise config.numItems is used
 * @param size            minimum size for new items
 * @return LUIDs of all inserted items
 */
std::list<std::string> LocalTests::insertManyItems(CreateSource createSource, int startIndex, int numItems, int size) {
    std::list<std::string> luids;

    CPPUNIT_ASSERT(!config.m_templateItem.empty());
    CPPUNIT_ASSERT(!config.m_uniqueProperties.empty());

    restoreStorage(config, client);
    TestingSyncSourcePtr source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceA()));
    CPPUNIT_ASSERT(startIndex > 1 || !countItems(source.get()));

    int firstIndex = startIndex;
    if (firstIndex < 0) {
        firstIndex = 1;
    }
    int lastIndex = firstIndex + (numItems >= 1 ? numItems : defNumItems()) - 1;
    for (int item = firstIndex; item <= lastIndex; item++) {
        std::string data = createItem(item, "", size);
        luids.push_back(importItem(source.get(), config, data));
    }
    backupStorage(config, client);

    return luids;
}

std::list<std::string> LocalTests::insertManyItems(TestingSyncSource *source, int startIndex, int numItems, int size) {
    std::list<std::string> luids;

    CPPUNIT_ASSERT(!config.m_templateItem.empty());
    CPPUNIT_ASSERT(!config.m_uniqueProperties.empty());

    CPPUNIT_ASSERT(startIndex > 1 || !countItems(source));
    int firstIndex = startIndex;
    if (firstIndex < 0) {
        firstIndex = 1;
    }
    int lastIndex = firstIndex + (numItems >= 1 ? numItems : defNumItems()) - 1;
    for (int item = firstIndex; item <= lastIndex; item++) {
        std::string data = createItem(item, "", size);
        luids.push_back(importItem(source, config, data));
    }

    return luids;
}

// update every single item in the database
void LocalTests::updateData(CreateSource createSource) {
    // check additional requirements
    CPPUNIT_ASSERT(config.m_update);

    TestingSyncSourcePtr source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSource()));
    BOOST_FOREACH(const string &luid, source->getAllItems()) {
        string item;
        source->readItemRaw(luid, item);
        config.m_update(item);
        source->insertItemRaw(luid, item);
    }
    CPPUNIT_ASSERT_NO_THROW(source.reset());
}


// creating sync source
void LocalTests::testOpen() {
    // check requirements
    CPPUNIT_ASSERT(config.m_createSourceA);

    // Intentionally use the plain auto_ptr here and
    // call open directly. That way it is a bit more clear
    // what happens and where it fails, if it fails.
    std::auto_ptr<TestingSyncSource> source(createSourceA());
    // got a sync source?
    CPPUNIT_ASSERT(source.get() != 0);
    // can it be opened?
    SOURCE_ASSERT_NO_FAILURE(source.get(), source->open());
    // delete it
    CPPUNIT_ASSERT_NO_THROW(source.reset());
}

// restart scanning of items
void LocalTests::testIterateTwice() {
    // check requirements
    CPPUNIT_ASSERT(config.m_createSourceA);

    // open source
    TestingSyncSourcePtr source(createSourceA());
    SOURCE_ASSERT_MESSAGE(
        "iterating twice should produce identical results",
        source.get(),
        countItems(source.get()) == countItems(source.get()));
}

// insert one contact without clearing the source first
void LocalTests::testSimpleInsert() {
    // check requirements
    CPPUNIT_ASSERT(!config.m_insertItem.empty());
    CPPUNIT_ASSERT(!config.m_createSourceA.empty());

    CPPUNIT_ASSERT_NO_THROW(insert(createSourceA, config.m_insertItem));
}

// delete all items
void LocalTests::testLocalDeleteAll() {
    // check requirements
    CPPUNIT_ASSERT(!config.m_insertItem.empty());
    CPPUNIT_ASSERT(config.m_createSourceA);

    // make sure there is something to delete, then delete again
    CPPUNIT_ASSERT_NO_THROW(insert(createSourceA, config.m_insertItem));
    CPPUNIT_ASSERT_NO_THROW(deleteAll(createSourceA));
}

// clean database, then insert
void LocalTests::testComplexInsert() {
    testLocalDeleteAll();
    testSimpleInsert();
    testIterateTwice();
}

// clean database, insert item, update it
void LocalTests::testLocalUpdate() {
    // check additional requirements
    CPPUNIT_ASSERT(!config.m_updateItem.empty());

    testLocalDeleteAll();
    testSimpleInsert();
    CPPUNIT_ASSERT_NO_THROW(update(createSourceA, config.m_updateItem));
}

// complex sequence of changes
void LocalTests::testChanges() {
    SyncSourceChanges::Items_t::const_iterator it, it2;

    // check additional requirements
    CPPUNIT_ASSERT(config.m_createSourceB);

    testLocalDeleteAll();
    testSimpleInsert();

    // clean changes in sync source B by creating and closing it
    TestingSyncSourcePtr source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceB()));
    CPPUNIT_ASSERT_NO_THROW(source.reset());

    // no new changes now
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
    string item;
    string luid;
    SOURCE_ASSERT_NO_FAILURE(source.get(), it = source->getAllItems().begin());
    CPPUNIT_ASSERT(it != source->getAllItems().end());
    luid = *it;
    // It is not required for incremental syncing that sources must be
    // able to return unchanged items. For example, ActiveSyncSource doesn't support
    // it because it gets only IDs and data of added or updated items.
    // Don't test it.
    // SOURCE_ASSERT_NO_FAILURE(source.get(), source->readItem(*it, item));
    CPPUNIT_ASSERT_NO_THROW(source.reset());

    // delete item again via sync source A
    CPPUNIT_ASSERT_NO_THROW(deleteAll(createSourceA));
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countDeletedItems(source.get()));
    SOURCE_ASSERT_NO_FAILURE(source.get(), it = source->getDeletedItems().begin());
    CPPUNIT_ASSERT(it != source->getDeletedItems().end());
    CPPUNIT_ASSERT(!it->empty());
    CPPUNIT_ASSERT_EQUAL(luid, *it);
    CPPUNIT_ASSERT_NO_THROW(source.reset());

    // insert another item via sync source A
    testSimpleInsert();
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
    SOURCE_ASSERT_NO_FAILURE(source.get(), it = source->getAllItems().begin());
    CPPUNIT_ASSERT(it != source->getAllItems().end());
    luid = *it;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source->readItem(*it, item));
    string newItem;
    SOURCE_ASSERT_NO_FAILURE(source.get(), it = source->getNewItems().begin());
    CPPUNIT_ASSERT(it != source->getNewItems().end());
    SOURCE_ASSERT_NO_FAILURE(source.get(), source->readItem(*it, item));
    CPPUNIT_ASSERT_EQUAL(luid, *it);
    CPPUNIT_ASSERT_NO_THROW(source.reset());

    // update item via sync source A
    CPPUNIT_ASSERT_NO_THROW(update(createSourceA, config.m_updateItem));
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countUpdatedItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
    string updatedItem;
    SOURCE_ASSERT_NO_FAILURE(source.get(), it = source->getUpdatedItems().begin());
    CPPUNIT_ASSERT(it != source->getUpdatedItems().end());
    SOURCE_ASSERT_NO_FAILURE(source.get(), source->readItem(*it, updatedItem));
    CPPUNIT_ASSERT_EQUAL(luid, *it);
    CPPUNIT_ASSERT_NO_THROW(source.reset());

    // start anew, then create and update an item -> should only be listed as new
    // or updated, but not both
    CPPUNIT_ASSERT_NO_THROW(deleteAll(createSourceA));
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceB()));
    CPPUNIT_ASSERT_NO_THROW(source.reset());
    testSimpleInsert();
    CPPUNIT_ASSERT_NO_THROW(update(createSourceA, config.m_updateItem));
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()) + countUpdatedItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
    CPPUNIT_ASSERT_NO_THROW(source.reset());

    // start anew, then create, delete and recreate an item -> should only be listed as new or updated,
    // even if (as for calendar with UID) the same LUID gets reused
    CPPUNIT_ASSERT_NO_THROW(deleteAll(createSourceA));
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceB()));
    CPPUNIT_ASSERT_NO_THROW(source.reset());
    testSimpleInsert();
    CPPUNIT_ASSERT_NO_THROW(deleteAll(createSourceA));
    testSimpleInsert();
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()) + countUpdatedItems(source.get()));
    if (countDeletedItems(source.get()) == 1) {
        // It's not nice, but acceptable to send the LUID of a deleted item to a
        // server which has never seen that LUID. The LUID must not be the same as
        // the one we list as new or updated, though.
        SOURCE_ASSERT_NO_FAILURE(source.get(), it = source->getDeletedItems().begin());
        CPPUNIT_ASSERT(it != source->getDeletedItems().end());
        SOURCE_ASSERT_NO_FAILURE(source.get(), it2 = source->getNewItems().begin());
        if (it2 == source->getNewItems().end()) {
            SOURCE_ASSERT_NO_FAILURE(source.get(), it2 = source->getUpdatedItems().begin());
            CPPUNIT_ASSERT(it2 != source->getUpdatedItems().end());
        }
        CPPUNIT_ASSERT(*it != *it2);
    } else {
        SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
    }
}

// clean database, import file, then export again and compare
void LocalTests::testImport() {
    // check additional requirements
    CPPUNIT_ASSERT(config.m_import);
    CPPUNIT_ASSERT(config.m_dump);
    CPPUNIT_ASSERT(config.m_compare);
    CPPUNIT_ASSERT(!config.m_testcases.empty());

    testLocalDeleteAll();

    // import via sync source A
    TestingSyncSourcePtr source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceA()));
    restoreStorage(config, client);
    std::string testcases;
    std::string importFailures = config.m_import(client, *source.get(), config, config.m_testcases, testcases);
    backupStorage(config, client);
    CPPUNIT_ASSERT_NO_THROW(source.reset());

    // export again and compare against original file
    TestingSyncSourcePtr copy;
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceA()));
    bool equal = compareDatabases(testcases.c_str(), *copy.get(), false);
    CPPUNIT_ASSERT_NO_THROW(source.reset());

    if (importFailures.empty()) {
        CPPUNIT_ASSERT_MESSAGE("imported and exported data equal", equal);
    } else {
        CPPUNIT_ASSERT_EQUAL(std::string(""), importFailures);
    }
}

// same as testImport() with immediate delete
void LocalTests::testImportDelete() {
    testImport();

    // delete again, because it was observed that this did not
    // work right with calendars in SyncEvolution
    testLocalDeleteAll();
}

// test change tracking with large number of items
void LocalTests::testManyChanges() {
    // check additional requirements
    CPPUNIT_ASSERT(!config.m_templateItem.empty());
    CPPUNIT_ASSERT(!config.m_uniqueProperties.empty());

    CPPUNIT_ASSERT_NO_THROW(deleteAll(createSourceA));

    // check that everything is empty, also resets change counter of sync source B
    TestingSyncSourcePtr copy;
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // now insert plenty of items
    int numItems;
    CPPUNIT_ASSERT_NO_THROW(numItems = insertManyItems(createSourceA).size());

    // check that exactly this number of items is listed as new
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), numItems, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), numItems, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // delete all items
    CPPUNIT_ASSERT_NO_THROW(deleteAll(createSourceA));

    // verify again
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), numItems, countDeletedItems(copy.get()));
    CPPUNIT_ASSERT_NO_THROW(copy.reset());
}

template<class T, class V> int countEqual(const T &container,
                                          const V &value) {
    return count(container.begin(),
                 container.end(),
                 value);
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsParent() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CPPUNIT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string parentData;
    TestingSyncSourcePtr copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // now insert main item
    CPPUNIT_ASSERT_NO_THROW(parent = insert(createSourceA, items[0], false, &parentData));

    // check that exactly the parent is listed as new
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CPPUNIT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    }
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    if (getenv("CLIENT_TEST_LINKED_ITEMS_NO_DELETE")) {
        return;
    }

    // delete all items
    CPPUNIT_ASSERT_NO_THROW(deleteAll(createSourceA));

    // verify again
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    }
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsChild() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CPPUNIT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string childData;
    TestingSyncSourcePtr copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // same as above for child item
    CPPUNIT_ASSERT_NO_THROW(child = insert(createSourceA, items[1], false, &childData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CPPUNIT_ASSERT_NO_THROW(compareDatabases(*copy, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
    }
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    if (getenv("CLIENT_TEST_LINKED_ITEMS_NO_DELETE")) {
        return;
    }

    CPPUNIT_ASSERT_NO_THROW(deleteAll(createSourceA));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
    }
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsParentChild() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CPPUNIT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string parentData, childData;
    TestingSyncSourcePtr copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // insert parent first, then child
    CPPUNIT_ASSERT_NO_THROW(parent = insert(createSourceA, items[0], false, &parentData));
    CPPUNIT_ASSERT_NO_THROW(child = insert(createSourceA, items[1], false, &childData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CPPUNIT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    }
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    if (getenv("CLIENT_TEST_LINKED_ITEMS_NO_DELETE")) {
        return;
    }

    CPPUNIT_ASSERT_NO_THROW(deleteAll(createSourceA));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    }
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsChildParent() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CPPUNIT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string parentData, childData;
    TestingSyncSourcePtr copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // insert child first, then parent
    CPPUNIT_ASSERT_NO_THROW(child = insert(createSourceA, items[1], false, &parentData));
    CPPUNIT_ASSERT_NO_THROW(parent = insert(createSourceA, items[0], true, &childData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CPPUNIT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    }
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    if (getenv("CLIENT_TEST_LINKED_ITEMS_NO_DELETE")) {
        return;
    }

    CPPUNIT_ASSERT_NO_THROW(deleteAll(createSourceA));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    }
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsChildChangesParent() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CPPUNIT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string parentData, childData;
    TestingSyncSourcePtr copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // insert child first, check changes, then insert the parent
    CPPUNIT_ASSERT_NO_THROW(child = insert(createSourceA, items[1], false, &childData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CPPUNIT_ASSERT_NO_THROW(compareDatabases(*copy, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
    }
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    CPPUNIT_ASSERT_NO_THROW(parent = insert(createSourceA, items[0], true, &parentData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CPPUNIT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countNewItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listNewItems(copy.get()), parent));
    }
    // relaxed semantic: the child item might be considered updated now if
    // it had to be modified when inserting the parent
    SOURCE_ASSERT(copy.get(), 1 >= countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    }
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    if (getenv("CLIENT_TEST_LINKED_ITEMS_NO_DELETE")) {
        return;
    }

    CPPUNIT_ASSERT_NO_THROW(deleteAll(createSourceA));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    }
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsRemoveParentFirst() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CPPUNIT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string parentData, childData;
    TestingSyncSourcePtr copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // insert both items, remove parent, then child
    CPPUNIT_ASSERT_NO_THROW(parent = insert(createSourceA, items[0], false, &parentData));
    CPPUNIT_ASSERT_NO_THROW(child = insert(createSourceA, items[1], false, &childData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CPPUNIT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    }
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    CPPUNIT_ASSERT_NO_THROW(deleteItem(createSourceA, parent));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CPPUNIT_ASSERT_NO_THROW(compareDatabases(*copy, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    // deleting the parent may or may not modify the child
    SOURCE_ASSERT(copy.get(), 1 >= countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    }
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    if (getenv("CLIENT_TEST_LINKED_ITEMS_NO_DELETE")) {
        return;
    }

    CPPUNIT_ASSERT_NO_THROW(deleteItem(createSourceA, child));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
    }
    CPPUNIT_ASSERT_NO_THROW(copy.reset());
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsRemoveNormal() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CPPUNIT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string parentData, childData;
    TestingSyncSourcePtr source, copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // insert both items, remove child, then parent
    CPPUNIT_ASSERT_NO_THROW(parent = insert(createSourceA, items[0], false, &parentData));
    CPPUNIT_ASSERT_NO_THROW(child = insert(createSourceA, items[1], false, &childData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CPPUNIT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    }
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    CPPUNIT_ASSERT_NO_THROW(deleteItem(createSourceA, child));

    // The removal of the child fails with Exchange (BMC #22849).
    // Skip the testing, proceed to full removal.
    if (currentServer() != "exchange") {
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceA()));
        if (getCurrentTest().find("::eds_event::") != std::string::npos) {
            // hack: ignore EDS side effect of adding EXDATE to parent, see http://bugs.meego.com/show_bug.cgi?id=10906
            size_t pos = parentData.rfind("DTSTART");
            parentData.insert(pos, "EXDATE:20080413T090000\n");
        }
        CPPUNIT_ASSERT_NO_THROW(compareDatabases(*source, &parentData, NULL));
        SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
        SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
        SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
        SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));

        CPPUNIT_ASSERT_NO_THROW(source.reset());

        SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
        SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
        // parent might have been updated
        int updated;
        CPPUNIT_ASSERT_NO_THROW(updated = countUpdatedItems(copy.get()));
        SOURCE_ASSERT(copy.get(), 0 <= updated && updated <= 1);
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
        if (!config.m_sourceLUIDsAreVolatile) {
            SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
        }
        CPPUNIT_ASSERT_NO_THROW(copy.reset());
    }

    CPPUNIT_ASSERT_NO_THROW(deleteItem(createSourceA, parent));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(),
                        // Exchange did not actually remove child above, done now.
                        currentServer() != "exchange" ? 1 : 2,
                        countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    }
    CPPUNIT_ASSERT_NO_THROW(copy.reset());
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsInsertParentTwice() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CPPUNIT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string parentData, childData;
    TestingSyncSourcePtr copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // add parent twice (should be turned into update)
    CPPUNIT_ASSERT_NO_THROW(parent = insert(createSourceA, items[0], false, &parentData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CPPUNIT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    }
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    CPPUNIT_ASSERT_NO_THROW(parent = insert(createSourceA, items[0], false, &parentData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CPPUNIT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listUpdatedItems(copy.get()), parent));
    }
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    if (getenv("CLIENT_TEST_LINKED_ITEMS_NO_DELETE")) {
        return;
    }

    CPPUNIT_ASSERT_NO_THROW(deleteItem(createSourceA, parent));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    }
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsInsertChildTwice() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CPPUNIT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string parentData, childData;
    TestingSyncSourcePtr copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // add child twice (should be turned into update)
    CPPUNIT_ASSERT_NO_THROW(child = insert(createSourceA, items[1], false, &childData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CPPUNIT_ASSERT_NO_THROW(compareDatabases(*copy, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
    }
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    CPPUNIT_ASSERT_NO_THROW(child = insert(createSourceA, items[1]));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CPPUNIT_ASSERT_NO_THROW(compareDatabases(*copy, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listUpdatedItems(copy.get()), child));
    }
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    if (getenv("CLIENT_TEST_LINKED_ITEMS_NO_DELETE")) {
        return;
    }

    CPPUNIT_ASSERT_NO_THROW(deleteItem(createSourceA, child));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
    }
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsParentUpdate() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CPPUNIT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string parentData, childData;
    TestingSyncSourcePtr copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // add parent, then update it
    CPPUNIT_ASSERT_NO_THROW(parent = insert(createSourceA, items[0], false, &parentData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CPPUNIT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    }
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    CPPUNIT_ASSERT_NO_THROW(parent = updateItem(createSourceA, config, parent, items[0], &parentData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CPPUNIT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listUpdatedItems(copy.get()), parent));
    }
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    if (getenv("CLIENT_TEST_LINKED_ITEMS_NO_DELETE")) {
        return;
    }

    CPPUNIT_ASSERT_NO_THROW(deleteItem(createSourceA, parent));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    }
    CPPUNIT_ASSERT_NO_THROW(copy.reset());
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsUpdateChild() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CPPUNIT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string parentData, childData;
    TestingSyncSourcePtr copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // add child, then update it
    CPPUNIT_ASSERT_NO_THROW(child = insert(createSourceA, items[1], false, &childData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CPPUNIT_ASSERT_NO_THROW(compareDatabases(*copy, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
    }
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    CPPUNIT_ASSERT_NO_THROW(child = updateItem(createSourceA, config, child, items[1], &childData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CPPUNIT_ASSERT_NO_THROW(compareDatabases(*copy, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listUpdatedItems(copy.get()), child));
    }
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    if (getenv("CLIENT_TEST_LINKED_ITEMS_NO_DELETE")) {
        return;
    }

    CPPUNIT_ASSERT_NO_THROW(deleteItem(createSourceA, child));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
    }
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsInsertBothUpdateChild() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CPPUNIT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string parentData, childData;
    TestingSyncSourcePtr copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // add parent and child, then update child
    CPPUNIT_ASSERT_NO_THROW(parent = insert(createSourceA, items[0], false, &parentData));
    CPPUNIT_ASSERT_NO_THROW(child = insert(createSourceA, items[1], false, &childData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CPPUNIT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    }
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    CPPUNIT_ASSERT_NO_THROW(child = updateItem(createSourceA, config, child, items[1], &childData));

    // child has to be listed as modified, parent may be
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CPPUNIT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT(copy.get(), 1 <= countUpdatedItems(copy.get()));
    SOURCE_ASSERT(copy.get(), 2 >= countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listUpdatedItems(copy.get()), child));
    }
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    if (getenv("CLIENT_TEST_LINKED_ITEMS_NO_DELETE")) {
        return;
    }

    CPPUNIT_ASSERT_NO_THROW(deleteItem(createSourceA, parent));
    CPPUNIT_ASSERT_NO_THROW(deleteItem(createSourceA, child));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
    }
    CPPUNIT_ASSERT_NO_THROW(copy.reset());
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsInsertBothUpdateParent() {
    ClientTestConfig::LinkedItems_t items = getParentChildData();

    CPPUNIT_ASSERT_NO_THROW(deleteAll(createSourceA));
    std::string parent, child;
    std::string parentData, childData;
    TestingSyncSourcePtr copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // add parent and child, then update parent
    CPPUNIT_ASSERT_NO_THROW(parent = insert(createSourceA, items[0], false, &parentData));
    CPPUNIT_ASSERT_NO_THROW(child = insert(createSourceA, items[1], false, &childData));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CPPUNIT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    }
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    CPPUNIT_ASSERT_NO_THROW(parent = updateItem(createSourceA, config, parent, items[0], &parentData));

    // parent has to be listed as modified, child may be
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    CPPUNIT_ASSERT_NO_THROW(compareDatabases(*copy, &parentData, &childData, NULL));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT(copy.get(), 1 <= countUpdatedItems(copy.get()));
    SOURCE_ASSERT(copy.get(), 2 >= countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listUpdatedItems(copy.get()), parent));
    }
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    if (getenv("CLIENT_TEST_LINKED_ITEMS_NO_DELETE")) {
        return;
    }

    CPPUNIT_ASSERT_NO_THROW(deleteItem(createSourceA, parent));
    CPPUNIT_ASSERT_NO_THROW(deleteItem(createSourceA, child));

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countDeletedItems(copy.get()));
    if (!config.m_sourceLUIDsAreVolatile) {
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
        SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
    }
}

ClientTestConfig::LinkedItems_t LocalTests::getParentChildData()
{
    // extract _%d suffix and use it as index for our config
    std::string test = getCurrentTest();
    const std::string testname = "LinkedItems_";
    size_t off = test.find(testname);
    CPPUNIT_ASSERT(off != test.npos);
    int i = atoi(test.c_str() + off + testname.size());
    CPPUNIT_ASSERT(i >= 0 && i < (int)config.m_linkedItems.size());
    CPPUNIT_ASSERT(config.m_linkedItems[i].size() >= 2);
    return config.m_linkedItems[i];
}

SyncTests::SyncTests(const std::string &name, ClientTest &cl, std::vector<int> sourceIndices, bool isClientA) :
    CppUnit::TestSuite(name),
    client(cl) {
    sourceArray = new int[sourceIndices.size() + 1];
    int offset = 0;
    for (std::vector<int>::iterator it = sourceIndices.begin();
         it != sourceIndices.end();
         ++it) {
        ClientTest::Config config;
        client.getSyncSourceConfig(*it, config);

        if (!config.m_sourceName.empty()) {
            sourceArray[sources.size()+offset] = *it;
            if (!config.m_subConfigs.empty()) {
                vector<string> subs;
                boost::split (subs, config.m_subConfigs, boost::is_any_of(","));
                offset++;
                ClientTest::Config subConfig;
                BOOST_FOREACH (string sub, subs) {
                client.getSourceConfig (sub, subConfig);
                sources.push_back(std::pair<int,LocalTests *>(*it, cl.createLocalTests(sub, client.getLocalSourcePosition(sub), subConfig)));
                offset--;
                }
            } else {
                sources.push_back(std::pair<int,LocalTests *>(*it, cl.createLocalTests(config.m_sourceName, client.getLocalSourcePosition(config.m_sourceName), config)));
            }
        }
    }
    sourceArray[sources.size()+ offset] = -1;

    // check whether we have a second client
    ClientTest *clientB = cl.getClientB();
    if (clientB) {
        accessClientB = clientB->createSyncTests(name, sourceIndices, false);
    } else {
        accessClientB = 0;
    }
}

SyncTests::~SyncTests() {
    for (source_it it = sources.begin();
         it != sources.end();
         ++it) {
        delete it->second;
    }
    delete [] sourceArray;
    if (accessClientB) {
        delete accessClientB;
    }
}

/** adds the supported tests to the instance itself */
void SyncTests::addTests(bool isFirstSource) {
    if (sources.size()) {
        const ClientTest::Config &config(sources[0].second->config);

        // run this test first, even if it is more complex:
        // if it works, all the following tests will run with
        // the server in a deterministic state
        if (config.m_createSourceA) {
            if (!config.m_insertItem.empty()) {
                ADD_TEST(SyncTests, testDeleteAllRefresh);
            }
        }

        ADD_TEST(SyncTests, testTwoWaySync);
        ADD_TEST(SyncTests, testSlowSync);
        ADD_TEST(SyncTests, testRefreshFromServerSync);
        ADD_TEST(SyncTests, testRefreshFromClientSync);
        // testTimeout is independent of the actual peer; all it needs
        // is a SyncML client config. Can't test for that explicitly
        // here, so only rule out the test if we run in server mode.
        if (isFirstSource &&
            (!getenv("CLIENT_TEST_MODE") ||
             strcmp(getenv("CLIENT_TEST_MODE"), "server"))) {
            ADD_TEST(SyncTests, testTimeout);
        }

        if (config.m_compare &&
            !config.m_testcases.empty() &&
            !isServerMode()) {
            ADD_TEST(SyncTests, testConversion);
        }

        if (config.m_createSourceA) {
            if (!config.m_insertItem.empty()) {
                ADD_TEST(SyncTests, testRefreshFromServerSemantic);
                ADD_TEST(SyncTests, testRefreshFromClientSemantic);
                ADD_TEST(SyncTests, testRefreshStatus);

                if (accessClientB &&
                    config.m_dump &&
                    config.m_compare) {
                    ADD_TEST(SyncTests, testCopy);
                    ADD_TEST(SyncTests, testDelete);
                    ADD_TEST(SyncTests, testAddUpdate);
                    ADD_TEST(SyncTests, testManyItems);
                    ADD_TEST(SyncTests, testManyDeletes);
                    ADD_TEST(SyncTests, testSlowSyncSemantic);
                    ADD_TEST(SyncTests, testComplexRefreshFromServerSemantic);
                    ADD_TEST(SyncTests, testDeleteBothSides);

                    // only add when testing individual source,
                    // test data not guaranteed to be available for all sources
                    if (sources.size() == 1 &&
                        !config.m_linkedItems.empty()) {
                        ADD_TEST(SyncTests, testLinkedItemsParentChild);

                        if (config.m_linkedItemsRelaxedSemantic) {
                            ADD_TEST(SyncTests, testLinkedItemsChild);
                            ADD_TEST(SyncTests, testLinkedItemsChildParent);
                        }
                    }

                    if (!config.m_updateItem.empty()) {
                        ADD_TEST(SyncTests, testUpdate);
                    }
                    if (!config.m_complexUpdateItem.empty()) {
                        ADD_TEST(SyncTests, testComplexUpdate);
                    }
                    if (!config.m_mergeItem1.empty() &&
                        !config.m_mergeItem2.empty()) {
                        ADD_TEST(SyncTests, testMerge);
                    }
                    if (config.m_import) {
                        ADD_TEST(SyncTests, testTwinning);
                        ADD_TEST(SyncTests, testItems);
                        ADD_TEST(SyncTests, testItemsXML);
                        if (config.m_update) {
                            ADD_TEST(SyncTests, testExtensions);
                        }
                    }
                    if (!config.m_templateItem.empty()) {
                        ADD_TEST(SyncTests, testMaxMsg);
                        ADD_TEST(SyncTests, testLargeObject);
                        ADD_TEST(SyncTests, testOneWayFromServer);
                        ADD_TEST(SyncTests, testOneWayFromClient);
                    }
                }
            }
        }

        if (config.m_retrySync &&
            !config.m_insertItem.empty() &&
            !config.m_updateItem.empty() &&
            accessClientB &&
            config.m_dump &&
            config.m_compare) {
            CppUnit::TestSuite *retryTests = new CppUnit::TestSuite(getName() + "::Retry");
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeClientAdd);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeClientRemove);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeClientUpdate);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeServerAdd);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeServerRemove);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeServerUpdate);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeClientAddBig);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeClientUpdateBig);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeServerAddBig);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeServerUpdateBig);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeFull);
            addTest(FilterTest(retryTests));
        }

        if (config.m_suspendSync &&
            !config.m_insertItem.empty() &&
            !config.m_updateItem.empty() &&
            accessClientB &&
            config.m_dump &&
            config.m_compare) {
            CppUnit::TestSuite *suspendTests = new CppUnit::TestSuite(getName() + "::Suspend");
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendClientAdd);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendClientRemove);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendClientUpdate);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendServerAdd);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendServerRemove);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendServerUpdate);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendClientAddBig);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendClientUpdateBig);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendServerAddBig);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendServerUpdateBig);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendFull);
            addTest(FilterTest(suspendTests));
        }

        if (config.m_resendSync &&
            !config.m_insertItem.empty() &&
            !config.m_updateItem.empty() &&
            accessClientB &&
            config.m_dump &&
            config.m_compare) {
            CppUnit::TestSuite *resendTests = new CppUnit::TestSuite(getName() + "::Resend");
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendClientAdd);
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendClientRemove);
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendClientUpdate);
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendServerAdd);
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendServerRemove);
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendServerUpdate);
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendFull);
            addTest(FilterTest(resendTests));
        }

        if (getenv("CLIENT_TEST_RESEND_PROXY") &&
            !config.m_insertItem.empty() &&
            !config.m_updateItem.empty() &&
            accessClientB &&
            config.m_dump &&
            config.m_compare) {
            CppUnit::TestSuite *resendTests = new CppUnit::TestSuite(getName() + "::ResendProxy");
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendProxyClientAdd);
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendProxyClientRemove);
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendProxyClientUpdate);
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendProxyServerAdd);
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendProxyServerRemove);
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendProxyServerUpdate);
            ADD_TEST_TO_SUITE(resendTests, SyncTests, testResendProxyFull);
            addTest(FilterTest(resendTests));
        }
    }
}

bool SyncTests::compareDatabases(const char *refFileBase, bool raiseAssert) {
    source_it it1;
    source_it it2;
    bool equal = true;

    CPPUNIT_ASSERT(accessClientB);
    for (it1 = sources.begin(), it2 = accessClientB->sources.begin();
         it1 != sources.end() && it2 != accessClientB->sources.end();
         ++it1, ++it2) {
        TestingSyncSourcePtr copy;
        SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(it2->second->createSourceB()));
        if (refFileBase) {
            std::string refFile = refFileBase;
            refFile += it1->second->config.m_sourceName;
            refFile += ".dat";
            simplifyFilename(refFile);
            if (!it1->second->compareDatabases(refFile.c_str(), *copy.get(), raiseAssert)) {
                equal = false;
            }
        } else {
            if (!it1->second->compareDatabases(NULL, *copy.get(), raiseAssert)) {
                equal = false;
            }
        }
        CPPUNIT_ASSERT_NO_THROW(copy.reset());
    }
    CPPUNIT_ASSERT(it1 == sources.end());
    CPPUNIT_ASSERT(it2 == accessClientB->sources.end());

    CPPUNIT_ASSERT(!raiseAssert || equal);
    return equal;
}

/** deletes all items locally and on server */
void SyncTests::deleteAll(DeleteAllMode mode) {
    source_it it;
    SyncPrefix prefix("deleteall", *this);

    const char *value = getenv ("CLIENT_TEST_DELETE_REFRESH");
    if (value) {
        mode = DELETE_ALL_REFRESH;
    }

    switch(mode) {
     case DELETE_ALL_SYNC:
        // a refresh from server would slightly reduce the amount of data exchanged, but not all servers support it
        for (it = sources.begin(); it != sources.end(); ++it) {
            it->second->deleteAll(it->second->createSourceA);
        }
        doSync("init", SyncOptions(SYNC_SLOW));
        // now that client and server are in sync, delete locally and sync again
        for (it = sources.begin(); it != sources.end(); ++it) {
            it->second->deleteAll(it->second->createSourceA);
        }
        doSync("twoway",
               SyncOptions(SYNC_TWO_WAY,
                           CheckSyncReport(0,0,0, 0,0,-1, true, SYNC_TWO_WAY)));
        break;
     case DELETE_ALL_REFRESH:
        // delete locally and then tell the server to "copy" the empty databases
        for (it = sources.begin(); it != sources.end(); ++it) {
            it->second->deleteAll(it->second->createSourceA);
        }
        doSync("refreshserver",
               SyncOptions(RefreshFromLocalMode(),
                           CheckSyncReport(0,0,0, 0,0,-1, true, RefreshFromLocalMode())));
        break;
    }
}

/** get both clients in sync with empty server, then copy one item from client A to B */
void SyncTests::doCopy() {
    SyncPrefix("copy", *this);

    // check requirements
    CPPUNIT_ASSERT(accessClientB);

    deleteAll();
    accessClientB->deleteAll();

    // insert into first database, copy to server
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->testSimpleInsert();
    }
    doSync("send",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 1,0,0, true, SYNC_TWO_WAY)));

    // copy into second database
    accessClientB->doSync("recv",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(1,0,0, 0,0,0, true, SYNC_TWO_WAY)));

    compareDatabases();
}

/**
 * replicate server database locally: same as SYNC_REFRESH_FROM_SERVER,
 * but done with explicit local delete and then a SYNC_SLOW because some
 * servers do no support SYNC_REFRESH_FROM_SERVER
 */
void SyncTests::refreshClient(SyncOptions options) {
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->deleteAll(it->second->createSourceA);
    }

    doSync("refresh",
           options
           .setSyncMode(SYNC_SLOW)
           .setCheckReport(CheckSyncReport(-1,0,0, 0,0,0, true, SYNC_SLOW)));
}


// delete all items, locally and on server using refresh-from-client sync
void SyncTests::testDeleteAllRefresh() {
    source_it it;

    // start with clean local data
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->deleteAll(it->second->createSourceA);
    }

    // copy something to server first; doesn't matter whether it has the
    // item already or not, as long as it exists there afterwards
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->testSimpleInsert();
    }
    doSync("insert", SyncOptions(SYNC_SLOW));

    // now ensure we can delete it
    deleteAll(DELETE_ALL_REFRESH);

    // nothing stored locally?
    for (it = sources.begin(); it != sources.end(); ++it) {
        TestingSyncSourcePtr source;
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceA()));
        SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
        CPPUNIT_ASSERT_NO_THROW(source.reset());
    }

    // make sure server really deleted everything
    doSync("check",
           SyncOptions(SYNC_SLOW,
                       CheckSyncReport(0,0,0, 0,0,0, true, SYNC_SLOW)));
    for (it = sources.begin(); it != sources.end(); ++it) {
        TestingSyncSourcePtr source;
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceA()));
        SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
        CPPUNIT_ASSERT_NO_THROW(source.reset());
    }
}

// test that a refresh sync from an empty server leads to an empty datatbase
// and no changes are sent to server during next two-way sync
void SyncTests::testRefreshFromServerSemantic() {
    source_it it;

    // clean client and server
    deleteAll();

    // insert item, then refresh from empty server
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->testSimpleInsert();
    }
    doSync("refresh",
           SyncOptions(RefreshFromPeerMode(),
                       CheckSyncReport(0,0,-1, 0,0,0, true, RefreshFromPeerMode())));

    // check
    for (it = sources.begin(); it != sources.end(); ++it) {
        TestingSyncSourcePtr source;
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceA()));
        SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
        CPPUNIT_ASSERT_NO_THROW(source.reset());
    }
    doSync("two-way",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,0,0, true, SYNC_TWO_WAY)));
}

// test that a refresh sync from an empty client leads to an empty datatbase
// and no changes are sent to server during next two-way sync
void SyncTests::testRefreshFromClientSemantic() {
    source_it it;

    // clean client and server
    deleteAll();

    // insert item, send to server
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->testSimpleInsert();
    }
    doSync("send",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 1,0,0, true, SYNC_TWO_WAY)));

    // delete locally
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->deleteAll(it->second->createSourceA);
    }

    // refresh from client
    doSync("refresh",
           SyncOptions(RefreshFromLocalMode(),
                       CheckSyncReport(0,0,0, 0,0,0, true, RefreshFromLocalMode())));

    // check
    doSync("check",
           SyncOptions(RefreshFromPeerMode(),
                       CheckSyncReport(0,0,0, 0,0,0, true, RefreshFromPeerMode())));
}

// tests the following sequence of events:
// - insert item
// - delete all items
// - insert one other item
// - refresh from client
// => no items should now be listed as new, updated or deleted for this client during another sync
void SyncTests::testRefreshStatus() {
    source_it it;

    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->testSimpleInsert();
    }
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->deleteAll(it->second->createSourceA);
    }
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->testSimpleInsert();
    }
    doSync("refresh-from-client",
           SyncOptions(RefreshFromLocalMode(),
                       CheckSyncReport(0,0,0, -1,-1,-1, /* strictly speaking 1,0,0, but not sure exactly what the server will be told */
                                       true, RefreshFromLocalMode())));
    doSync("two-way",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,0,0, true, SYNC_TWO_WAY)));
}

// test that a two-way sync copies updates from database to the other client,
// using simple data commonly supported by servers
void SyncTests::testUpdate() {
    CPPUNIT_ASSERT(sources.begin() != sources.end());
    CPPUNIT_ASSERT(!sources.begin()->second->config.m_updateItem.empty());

    // setup client A, B and server so that they all contain the same item
    doCopy();

    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->update(it->second->createSourceA, it->second->config.m_updateItem);
    }

    doSync("update",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,1,0, true, SYNC_TWO_WAY)));
    accessClientB->doSync("update",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(0,1,0, 0,0,0, true, SYNC_TWO_WAY)));

    CPPUNIT_ASSERT_NO_THROW(compareDatabases());
}

// test that a two-way sync copies updates from database to the other client,
// using data that some, but not all servers support, like adding a second
// phone number to a contact
void SyncTests::testComplexUpdate() {
    // setup client A, B and server so that they all contain the same item
    doCopy();

    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->update(it->second->createSourceA,
                           /* this test might get executed with some sources which have
                              a complex update item while others don't: use the normal update item
                              for them or even just the same item */
                           !it->second->config.m_complexUpdateItem.empty() ? it->second->config.m_complexUpdateItem :
                           !it->second->config.m_updateItem.empty() ? it->second->config.m_updateItem :
                           it->second->config.m_insertItem
                           );
    }

    doSync("update",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,1,0, true, SYNC_TWO_WAY)));
    accessClientB->doSync("update",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(0,1,0, 0,0,0, true, SYNC_TWO_WAY)));

    CPPUNIT_ASSERT_NO_THROW(compareDatabases());
}


// test that a two-way sync deletes the copy of an item in the other database
void SyncTests::testDelete() {
    // setup client A, B and server so that they all contain the same item
    doCopy();

    // delete it on A
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->deleteAll(it->second->createSourceA);
    }

    // transfer change from A to server to B
    doSync("delete",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,0,1, true, SYNC_TWO_WAY)));
    accessClientB->doSync("delete",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(0,0,1, 0,0,0, true, SYNC_TWO_WAY)));

    // check client B: shouldn't have any items now
    for (it = sources.begin(); it != sources.end(); ++it) {
        TestingSyncSourcePtr copy;
        SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(it->second->createSourceA()));
        SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
        CPPUNIT_ASSERT_NO_THROW(copy.reset());
    }
}

// test what the server does when it finds that different
// fields of the same item have been modified
void SyncTests::testMerge() {
    // setup client A, B and server so that they all contain the same item
    doCopy();

    // update in client A
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->update(it->second->createSourceA, it->second->config.m_mergeItem1);
    }

    // update in client B
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        it->second->update(it->second->createSourceA, it->second->config.m_mergeItem2);
    }

    // send change to server from client A (no conflict)
    doSync("update",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,1,0, true, SYNC_TWO_WAY)));
    // Now the changes from client B (conflict!).
    // There are several possible outcomes:
    // - client item completely replaces server item
    // - server item completely replaces client item (update on client)
    // - server merges and updates client
    accessClientB->doSync("conflict",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(-1,-1,-1, -1,-1,-1, true, SYNC_TWO_WAY)));

    // figure out how the conflict during ".conflict" was handled
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        TestingSyncSourcePtr copy;
        SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(it->second->createSourceA()));
        int numItems = 0;
        SOURCE_ASSERT_NO_FAILURE(copy.get(), numItems = countItems(copy.get()));
        CPPUNIT_ASSERT(numItems >= 1);
        CPPUNIT_ASSERT(numItems <= 2);
        std::cerr << " \"" << it->second->config.m_sourceName << ": " << (numItems == 1 ? "conflicting items were merged" : "both of the conflicting items were preserved") << "\" ";
        std::cerr.flush();
        CPPUNIT_ASSERT_NO_THROW(copy.reset());        
    }

    // now pull the same changes into client A
    doSync("refresh",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(-1,-1,-1, 0,0,0, true, SYNC_TWO_WAY)));

    // client A and B should have identical data now
    CPPUNIT_ASSERT_NO_THROW(compareDatabases());

    // Furthermore, it should be identical with the server.
    // Be extra careful and pull that data anew and compare once more.
    doSync("check",
           SyncOptions(RefreshFromPeerMode(),
                       CheckSyncReport(-1,-1,-1, -1,-1,-1, true, RefreshFromPeerMode())));
    CPPUNIT_ASSERT_NO_THROW(compareDatabases());
}

// test what the server does when it has to execute a slow sync
// with identical data on client and server:
// expected behaviour is that nothing changes
void SyncTests::testTwinning() {
    // clean server and client A
    deleteAll();

    // import test data
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->testImport();
    }

    // send to server
    doSync("send", SyncOptions(SYNC_TWO_WAY));

    // ensure that client has the same data, thus ignoring data conversion
    // issues (those are covered by testItems())
    refreshClient();

    // copy to client B to have another copy
    accessClientB->refreshClient();

    // slow sync should not change anything
    doSync("twinning", SyncOptions(SYNC_SLOW));

    // check
    CPPUNIT_ASSERT_NO_THROW(compareDatabases());
}

// tests one-way sync from server:
// - get both clients and server in sync with no items anywhere
// - add one item on first client, copy to server
// - add a different item on second client, one-way-from-server
// - two-way sync with first client
// => one item on first client, two on second
// - delete on first client, sync that to second client
//   via two-way sync + one-way-from-server
// => one item left on second client (the one inserted locally)
void SyncTests::testOneWayFromServer() {
    // no items anywhere
    deleteAll();
    accessClientB->refreshClient();

    // check that everything is empty, also resets change tracking
    // in second sources of each client
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }

    // add one item on first client, copy to server, and check change tracking via second source
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->insertManyItems(it->second->createSourceA, 200, 1);
    }
    doSync("send",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 1,0,0, true, SYNC_TWO_WAY)));
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }

    // add a different item on second client, one-way-from-server
    // => one item added locally, none sent to server
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        it->second->insertManyItems(it->second->createSourceA, 2, 1);

        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }
    accessClientB->doSync("recv",
                          SyncOptions(OneWayFromPeerMode(),
                                      CheckSyncReport(1,0,0, 0,0,0, true, OneWayFromPeerMode())));
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 2, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }

    // two-way sync with first client for verification
    // => no changes
    doSync("check",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,0,0, true, SYNC_TWO_WAY)));
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }

    // delete items on clientA, sync to server
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->deleteAll(it->second->createSourceA);

        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }
    doSync("delete",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,0,1, true, SYNC_TWO_WAY)));
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }

    // sync the same change to second client
    // => one item left (the one inserted locally)
    accessClientB->doSync("delete",
                          SyncOptions(OneWayFromPeerMode(),
                                      CheckSyncReport(0,0,1, 0,0,0, true, OneWayFromPeerMode())));
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }
}

// tests one-way sync from client:
// - get both clients and server in sync with no items anywhere
// - add one item on first client, copy to server
// - add a different item on second client, one-way-from-client
// - two-way sync with first client
// => two items on first client, one on second
// - delete on second client, sync that to first client
//   via one-way-from-client, two-way
// => one item left on first client (the one inserted locally)
void SyncTests::testOneWayFromClient() {
    // no items anywhere
    deleteAll();
    accessClientB->deleteAll();

    // check that everything is empty, also resets change tracking
    // in second sources of each client
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }

    // add one item on first client, copy to server, and check change tracking via second source
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->insertManyItems(it->second->createSourceA, 1, 1);
    }
    doSync("send",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 1,0,0, true, SYNC_TWO_WAY)));
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }

    // add a different item on second client, one-way-from-client
    // => no item added locally, one sent to server
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        it->second->insertManyItems(it->second->createSourceA, 2, 1);

        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }
    accessClientB->doSync("send",
                          SyncOptions(OneWayFromLocalMode(),
                                      CheckSyncReport(0,0,0, 1,0,0, true, OneWayFromLocalMode())));
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }

    // two-way sync with client A for verification
    // => receive one item
    doSync("check",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(1,0,0, 0,0,0, true, SYNC_TWO_WAY)));
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 2, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }

    // delete items on client B, sync to server
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        it->second->deleteAll(it->second->createSourceA);

        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }
    accessClientB->doSync("delete",
                          SyncOptions(OneWayFromLocalMode(),
                                      CheckSyncReport(0,0,0, 0,0,1, true, OneWayFromLocalMode())));
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }

    // sync the same change to client A
    // => one item left (the one inserted locally)
    doSync("delete",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,1, 0,0,0, true, SYNC_TWO_WAY)));
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }
}

// get engine ready, then use it to convert our test items
// to and from the internal field list
void SyncTests::testConversion() {
    bool success = false;
    SyncOptions::Callback_t callback = boost::bind(&SyncTests::doConversionCallback, this, &success, _1, _2);

    doSync(SyncOptions(SYNC_TWO_WAY, CheckSyncReport(-1,-1,-1, -1,-1,-1, false))
           .setStartCallback(callback));
    CPPUNIT_ASSERT(success);
}

bool SyncTests::doConversionCallback(bool *success,
                                     SyncContext &syncClient,
                                     SyncOptions &options) {
    *success = false;

    for (source_it it = sources.begin(); it != sources.end(); ++it) {
        const ClientTest::Config *config = &it->second->config;
        TestingSyncSource *source = static_cast<TestingSyncSource *>(syncClient.findSource(config->m_sourceName));
        CPPUNIT_ASSERT(source);

        std::string type = source->getNativeDatatypeName();
        if (type.empty()) {
            continue;
        }

        std::list<std::string> items;
        std::string testcases;
        ClientTest::getItems(config->m_testcases, items, testcases);
        std::string converted = getCurrentTest();
        converted += ".converted.";
        converted += config->m_sourceName;
        converted += ".dat";
        simplifyFilename(converted);
        std::ofstream out(converted.c_str());
        BOOST_FOREACH(const string &item, items) {
            string convertedItem = item;
            if(!sysync::DataConversion(syncClient.getSession().get(),
                                       type.c_str(),
                                       type.c_str(),
                                       convertedItem)) {
                SE_LOG_ERROR(NULL, NULL, "failed parsing as %s:\n%s",
                             type.c_str(),
                             item.c_str());
            } else {
                out << convertedItem << "\n";
            }
        }
        out.close();
        CPPUNIT_ASSERT(config->m_compare(client, testcases, converted));
    }

    // abort sync after completing the test successfully (no exception so far!)
    *success = true;
    return true;
}

// imports test data, transmits it from client A to the server to
// client B and then compares which of the data has been transmitted
void SyncTests::testItems() {
    // clean server and first test database
    deleteAll();

    // import data
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->testImport();
    }

    // transfer from client A to server to client B
    doSync("send", SyncOptions(SYNC_TWO_WAY).setWBXML(true));
    accessClientB->refreshClient(SyncOptions().setWBXML(true));

    CPPUNIT_ASSERT_NO_THROW(compareDatabases());
}

// creates several items, transmits them back and forth and
// then compares which of them have been preserved
void SyncTests::testItemsXML() {
    // clean server and first test database
    deleteAll();

    // import data
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->testImport();
    }

    // transfer from client A to server to client B using the non-default XML format
    doSync("send", SyncOptions(SYNC_TWO_WAY).setWBXML(false));
    accessClientB->refreshClient(SyncOptions().setWBXML(false));

    CPPUNIT_ASSERT_NO_THROW(compareDatabases());
}

// imports test data, transmits it from client A to the server to
// client B, update on B and transfers back to the server,
// then compares against reference data that has the same changes
// applied on A
void SyncTests::testExtensions() {
    // clean server and first test database
    deleteAll();

    // import data and create reference data
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->testImport();

        string refDir = getCurrentTest() + "." + it->second->config.m_sourceName + ".ref.dat";
        simplifyFilename(refDir);
        rm_r(refDir);
        mkdir_p(refDir);

        TestingSyncSourcePtr source;
        int counter = 0;
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
        BOOST_FOREACH(const string &luid, source->getAllItems()) {
            string item;
            source->readItemRaw(luid, item);
            it->second->config.m_update(item);
            ofstream out(StringPrintf("%s/%d", refDir.c_str(), counter).c_str());
            out.write(item.c_str(), item.size());
            counter++;
        }
        CPPUNIT_ASSERT_NO_THROW(source.reset());
    }

    // transfer from client A to server to client B
    doSync("send", SyncOptions(SYNC_TWO_WAY));
    accessClientB->refreshClient(SyncOptions());

    // update on client B
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        it->second->updateData(it->second->createSourceB);
    }

    // send back
    accessClientB->doSync("update", SyncOptions(SYNC_TWO_WAY));
    doSync("patch", SyncOptions(SYNC_TWO_WAY));

    // compare data in source A against reference data *without* telling synccompare
    // to ignore known data loss for the server
    ScopedEnvChange env("CLIENT_TEST_SERVER", "");
    ScopedEnvChange envParams("CLIENT_TEST_STRIP_PARAMETERS", "X-EVOLUTION-UI-SLOT");
    ScopedEnvChange envProps("CLIENT_TEST_STRIP_PROPERTIES", "(PHOTO|FN)");
    bool equal = true;
    for (it = sources.begin(); it != sources.end(); ++it) {
        string refDir = getCurrentTest() + "." + it->second->config.m_sourceName + ".ref.dat";
        simplifyFilename(refDir);
        TestingSyncSourcePtr source;
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
        if (!it->second->compareDatabases(refDir.c_str(), *source, false)) {
            equal = false;
        }
    }
    CPPUNIT_ASSERT(equal);
}

// tests the following sequence of events:
// - both clients in sync with server
// - client 1 adds item
// - client 1 updates the same item
// - client 2 gets item: the client should be asked to add the item
//
// However it has been observed that sometimes the item was sent as "update"
// for a non-existant local item. This is a server bug, the client does not
// have to handle that. See
// http://forge.objectweb.org/tracker/?func=detail&atid=100096&aid=305018&group_id=96
void SyncTests::testAddUpdate() {
    // clean server and both test databases
    deleteAll();
    accessClientB->refreshClient();

    // add item
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->insert(it->second->createSourceA, it->second->config.m_insertItem, false);
    }
    doSync("add",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 1,0,0, true, SYNC_TWO_WAY)));

    // update it
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->update(it->second->createSourceB, it->second->config.m_updateItem);
    }
    doSync("update",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,1,0, true, SYNC_TWO_WAY)));

    // now download the updated item into the second client
    accessClientB->doSync("recv",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(1,0,0, 0,0,0, true, SYNC_TWO_WAY)));

    // compare the two databases
    CPPUNIT_ASSERT_NO_THROW(compareDatabases());
}

//
// stress tests: execute some of the normal operations,
// but with large number of artificially generated items
//

// two-way sync with clean client/server,
// followed by slow sync and comparison
// via second client
void SyncTests::testManyItems() {
    // clean server and client A
    deleteAll();

    // import artificial data: make them large to generate some
    // real traffic and test buffer handling
    source_it it;
    int num_items = defNumItems();
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->insertManyItems(it->second->createSourceA, 0, num_items, 2000);
    }

    // send data to server
    doSync("send",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, num_items,0,0, true, SYNC_TWO_WAY),
                       SyncOptions::DEFAULT_MAX_MSG_SIZE,
                       SyncOptions::DEFAULT_MAX_OBJ_SIZE, 
                       true));

    // ensure that client has the same data, ignoring data conversion
    // issues (those are covered by testItems())
    refreshClient();

    // also copy to second client
    accessClientB->refreshClient();

    // slow sync now should not change anything
    doSync("twinning",
           SyncOptions(SYNC_SLOW,
                       CheckSyncReport(-1,-1,-1, -1,-1,-1, true, SYNC_SLOW),
                       SyncOptions::DEFAULT_MAX_MSG_SIZE,
                       SyncOptions::DEFAULT_MAX_OBJ_SIZE, 
                       true));

    // compare
    CPPUNIT_ASSERT_NO_THROW(compareDatabases());
}

/**
 * Tell server to delete plenty of items.
 */
void SyncTests::testManyDeletes() {
    // clean server and client A
    deleteAll();

    // import artificial data: make them small, we just want
    // many of them
    source_it it;
    int num_items = defNumItems();
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->insertManyItems(it->second->createSourceA, 0, num_items, 100);
    }

    // send data to server
    doSync("send",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, num_items,0,0, true, SYNC_TWO_WAY),
                       64 * 1024, 64 * 1024, true));

    // ensure that client has the same data, ignoring data conversion
    // issues (those are covered by testItems())
    refreshClient();

    // also copy to second client
    accessClientB->refreshClient();

    // slow sync now should not change anything
    doSync("twinning",
           SyncOptions(SYNC_SLOW,
                       CheckSyncReport(-1,-1,-1, -1,-1,-1, true, SYNC_SLOW),
                       64 * 1024, 64 * 1024, true));

    // compare
    CPPUNIT_ASSERT_NO_THROW(compareDatabases());

    // delete everything locally
    BOOST_FOREACH(source_array_t::value_type &source_pair, sources)  {
        source_pair.second->deleteAll(source_pair.second->createSourceA);
    }
    doSync("delete-server",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,0,num_items, true, SYNC_TWO_WAY),
                       10 * 1024));

    // Reporting locally deleted items depends on sync mode
    // recognition, see SyncContext.cpp.
    const char* checkSyncModeStr = getenv("CLIENT_TEST_NOCHECK_SYNCMODE");    

    // update second client
    accessClientB->doSync("delete-client",
                          SyncOptions(RefreshFromPeerMode(),
                                      checkSyncModeStr ? CheckSyncReport() :
                                      CheckSyncReport(0,0,num_items, 0,0,0, true, RefreshFromPeerMode()),
                                      10 * 1024));
}

/**
 * - get client A, server, client B in sync with one item
 * - force slow sync in A: must not duplicate items, but may update it locally
 * - refresh client B (in case that the item was updated)
 * - delete item in B and server via two-way sync
 * - refresh-from-server in B to check that item is gone
 * - two-way in A: must delete the item
 */
void SyncTests::testSlowSyncSemantic()
{
    // set up one item everywhere
    doCopy();

    // slow in A
    doSync("slow",
           SyncOptions(SYNC_SLOW,
                       CheckSyncReport(0,-1,0, -1,-1,0, true, SYNC_SLOW)));

    // refresh B, delete item
    accessClientB->doSync("refresh",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(0,-1,0, 0,0,0, true, SYNC_TWO_WAY)));
    BOOST_FOREACH(source_array_t::value_type &source_pair, accessClientB->sources)  {
        source_pair.second->deleteAll(source_pair.second->createSourceA);
    }
    accessClientB->doSync("delete",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(0,0,0, 0,0,1, true, SYNC_TWO_WAY)));
    accessClientB->doSync("check",
                          SyncOptions(RefreshFromPeerMode(),
                                      CheckSyncReport(0,0,0, 0,0,0, true, RefreshFromPeerMode())));

    // now the item should also be deleted on A
    doSync("delete",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,1, 0,0,0, true, SYNC_TWO_WAY)));
}

/**
 * check that refresh-from-server works correctly:
 * - create the same item on A, server, B via testCopy()
 * - refresh B (one item deleted, one created)
 * - delete item on A and server
 * - refresh B (one item deleted)
 */
void SyncTests::testComplexRefreshFromServerSemantic()
{
    testCopy();

    // Reporting locally deleted items depends on sync mode
    // recognition, see SyncContext.cpp.
    const char* checkSyncModeStr = getenv("CLIENT_TEST_NOCHECK_SYNCMODE");    

    // check refresh with one item on server
    const char *value = getenv ("CLIENT_TEST_NOREFRESH");
    // If refresh_from_server or refresh_from_client (depending on this is a
    // server or client) is not supported, we can still test via slow sync.
    if (value) {
        accessClientB->refreshClient();
    } else {
        accessClientB->doSync("refresh-one",
                              SyncOptions(RefreshFromPeerMode(),
                                          checkSyncModeStr ? CheckSyncReport() :
                                          CheckSyncReport(1,0,1, 0,0,0, true, RefreshFromPeerMode())));
    }

    // delete that item via A, check again
    BOOST_FOREACH(source_array_t::value_type &source_pair, sources)  {
        source_pair.second->deleteAll(source_pair.second->createSourceA);
    }
    doSync("delete-item",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,0,1, true, SYNC_TWO_WAY)));
    if (value) {
        accessClientB->refreshClient();
    } else {
        accessClientB->doSync("refresh-none",
                              SyncOptions(RefreshFromPeerMode(),
                                          checkSyncModeStr ? CheckSyncReport() :
                                          CheckSyncReport(0,0,1, 0,0,0, true, RefreshFromPeerMode())));
    }
}

/**
 * - create the same item on A, server, B via testCopy()
 * - delete on both sides
 * - sync A
 * - sync B
 *
 * Must not fail, even though the Synthesis engine will ask the backends
 * for deletion of an already deleted item.
 */
void SyncTests::testDeleteBothSides()
{
    testCopy();

    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->deleteAll(it->second->createSourceA);
    }
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        it->second->deleteAll(it->second->createSourceA);
    }

    doSync("delete-item-A",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,0,1, true, SYNC_TWO_WAY)));
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }


    // it is undefined whether the item is meant to be reported as deleted again here:
    // a SyncML client test will mark it as deleted, local sync as server won't
    accessClientB->doSync("delete-item-B",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(0,0,0, 0,0,-1, true, SYNC_TWO_WAY)));
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        if (it->second->config.m_createSourceB) {
            TestingSyncSourcePtr source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }
}

/**
 * - adds parent on client A
 * - syncs A
 * - adds unrelated item via client B (necessary to trigger corner cases in
 *   change tracking, see BMC #22329)
 * - syncs B
 * - adds child on client A
 * - syncs A and B
 * - compares
 */
void SyncTests::testLinkedItemsParentChild()
{
    source_it it;

    // clean server, client A and client B
    deleteAll();
    accessClientB->refreshClient();

    // create and copy parent item
    for (it = sources.begin(); it != sources.end(); ++it) {
        CPPUNIT_ASSERT(!it->second->config.m_linkedItems.empty());
        CPPUNIT_ASSERT(it->second->config.m_linkedItems[0].size() >= 2);
        TestingSyncSourcePtr source;
        CPPUNIT_ASSERT_NO_THROW(it->second->insert(it->second->createSourceA,
                                                   it->second->config.m_linkedItems[0][0],
                                                   false));
    }
    doSync("send-parent",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 1,0,0, true, SYNC_TWO_WAY)));

    // create independent item, refresh client B and server
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        CPPUNIT_ASSERT_NO_THROW(it->second->insert(it->second->createSourceA,
                                                   it->second->config.m_insertItem,
                                                   false));
    }
    accessClientB->doSync("recv-parent",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(1,0,0, 1,0,0, true, SYNC_TWO_WAY)));

    // add child on client A
    for (it = sources.begin(); it != sources.end(); ++it) {
        CPPUNIT_ASSERT(!it->second->config.m_linkedItems.empty());
        CPPUNIT_ASSERT(it->second->config.m_linkedItems[0].size() >= 2);
        TestingSyncSourcePtr source;
        CPPUNIT_ASSERT_NO_THROW(it->second->insert(it->second->createSourceA,
                                                   it->second->config.m_linkedItems[0][1],
                                                   false));
    }
    // parent may or may not be considered updated
    doSync("send-child",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(1,0,0, 1,-1,0, true, SYNC_TWO_WAY)));
    // parent may or may not be considered updated here
    accessClientB->doSync("recv-child",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(1,-1,0, 0,0,0, true, SYNC_TWO_WAY)));

    // final comparison
    CPPUNIT_ASSERT_NO_THROW(compareDatabases());
}

/**
 * - adds child on client A
 * - syncs A
 * - syncs B
 * - compare
 */
void SyncTests::testLinkedItemsChild()
{
    source_it it;

    // clean server, client A and client B
    deleteAll();
    accessClientB->refreshClient();

    // create and copy child item
    for (it = sources.begin(); it != sources.end(); ++it) {
        CPPUNIT_ASSERT(!it->second->config.m_linkedItems.empty());
        CPPUNIT_ASSERT(it->second->config.m_linkedItems[0].size() >= 2);
        TestingSyncSourcePtr source;
        CPPUNIT_ASSERT_NO_THROW(it->second->insert(it->second->createSourceA,
                                                   it->second->config.m_linkedItems[0][1],
                                                   false));
    }
    doSync("send",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 1,0,0, true, SYNC_TWO_WAY)));
    accessClientB->doSync("recv",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(1,0,0, 0,0,0, true, SYNC_TWO_WAY)));

    // final comparison
    CPPUNIT_ASSERT_NO_THROW(compareDatabases());
}


/**
 * - adds child on client A
 * - syncs A and B
 * - adds parent on client A
 * - syncs A and B
 * - compares
 */
void SyncTests::testLinkedItemsChildParent()
{
    source_it it;

    // clean server, client A and client B
    deleteAll();
    accessClientB->refreshClient();

    // create and copy child item
    for (it = sources.begin(); it != sources.end(); ++it) {
        CPPUNIT_ASSERT(!it->second->config.m_linkedItems[0].empty());
        CPPUNIT_ASSERT(it->second->config.m_linkedItems[0].size() >= 2);
        TestingSyncSourcePtr source;
        CPPUNIT_ASSERT_NO_THROW(it->second->insert(it->second->createSourceA,
                                                   it->second->config.m_linkedItems[0][1],
                                                   false));
    }
    doSync("send-child",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 1,0,0, true, SYNC_TWO_WAY)));
    accessClientB->doSync("recv-child",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(1,0,0, 0,0,0, true, SYNC_TWO_WAY)));

    // add parent on client A
    for (it = sources.begin(); it != sources.end(); ++it) {
        CPPUNIT_ASSERT(!it->second->config.m_linkedItems.empty());
        CPPUNIT_ASSERT(it->second->config.m_linkedItems[0].size() >= 2);
        TestingSyncSourcePtr source;
        CPPUNIT_ASSERT_NO_THROW(it->second->insert(it->second->createSourceA,
                                                   it->second->config.m_linkedItems[0][0],
                                                   false));
    }
    // child may or may not be considered updated
    doSync("send-parent",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 1,-1,0, true, SYNC_TWO_WAY)));
    // child may or may not be considered updated here
    accessClientB->doSync("recv-parent",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(1,-1,0, 0,0,0, true, SYNC_TWO_WAY)));

    // final comparison
    CPPUNIT_ASSERT_NO_THROW(compareDatabases());
}

/**
 * implements testMaxMsg(), testLargeObject(), testLargeObjectEncoded()
 * using a sequence of items with varying sizes
 */
void SyncTests::doVarSizes(bool withMaxMsgSize,
                           bool withLargeObject) {
    int maxMsgSize = 8 * 1024;
    const char* maxItemSize = getenv("CLIENT_TEST_MAX_ITEMSIZE");
    int tmpSize = maxItemSize ? atoi(maxItemSize) : 0;
    if(tmpSize > 0) 
        maxMsgSize = tmpSize;

    // clean server and client A
    deleteAll();

    // insert items, doubling their size, then restart with small size
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        int item = 1;
        restoreStorage(it->second->config, client);
        TestingSyncSourcePtr source;
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceA()));
        for (int i = 0; i < 2; i++ ) {
            int size = 1;
            while (size < 2 * maxMsgSize) {
                it->second->insertManyItems(source.get(), item, 1, it->second->config.m_templateItem.size() + 10 + size);
                size *= 2;
                item++;
            }
        }
        backupStorage(it->second->config, client);
    }

    // transfer to server
    doSync("send",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, -1,0,0, true, SYNC_TWO_WAY), // number of items sent to server depends on source
                       withMaxMsgSize ? SyncOptions::DEFAULT_MAX_MSG_SIZE: 0,
                       withMaxMsgSize ? SyncOptions::DEFAULT_MAX_OBJ_SIZE : 0,
                       withLargeObject));

    // copy to second client
    const char *value = getenv ("CLIENT_TEST_NOREFRESH");
    // If refresh_from_server or refresh_from_client (depending on this is a
    // server or client) is not supported, we can still test via slow sync.
    if (value) {
        accessClientB->refreshClient();
    } else {
        accessClientB->doSync("recv",
                SyncOptions(RefreshFromPeerMode(),
                    CheckSyncReport(-1,0,-1, 0,0,0, true, RefreshFromPeerMode()), // number of items received from server depends on source
                    withLargeObject ? maxMsgSize : withMaxMsgSize ? maxMsgSize * 100 /* large enough so that server can sent the largest item */ : 0,
                    withMaxMsgSize ? maxMsgSize * 100 : 0,
                    withLargeObject));
    }
    // compare
    CPPUNIT_ASSERT_NO_THROW(compareDatabases());
}

/**
 * Send message to server, then pretend that we timed out at exactly
 * one specific message, specified via m_interruptAtMessage.  The
 * caller is expected to resend the message, without aborting the
 * session. That resend and all following message will get through
 * again.
 *
 * Each send() is counted as one message, starting at 1 for the first
 * message.
 */
class TransportResendInjector : public TransportWrapper{
private:
    int timeout;
public:
    TransportResendInjector()
         :TransportWrapper() {
             const char *s = getenv("CLIENT_TEST_RESEND_TIMEOUT");
             timeout = s ? atoi(s) : 0;
    }

    ~TransportResendInjector() {
    }

    virtual int getResendFailureThreshold() { return 0; }

    virtual void send(const char *data, size_t len)
    {
        m_messageCount++;
        if (m_interruptAtMessage >= 0 &&
                m_messageCount == m_interruptAtMessage+1) {
            m_wrappedAgent->send(data, len);
            m_status = m_wrappedAgent->wait();
            //trigger client side resend
            sleep (timeout);
            m_status = TIME_OUT;
        }
        else 
        {
            m_wrappedAgent->send(data, len);
            m_status = m_wrappedAgent->wait();
        }
    }

    virtual void getReply(const char *&data, size_t &len, std::string &contentType) {
        if (m_status == FAILED) {
            data = "";
            len = 0;
        } else {
            m_wrappedAgent->getReply(data, len, contentType);
        }
    }
};

/**
 * Stop sending at m_interruptAtMessage. The caller is forced to abort
 * the current session and will recover by retrying in another
 * session.
 *
 * Each send() increments the counter by two, so that 1 aborts before
 * the first message and 2 after it.
 */
class TransportFaultInjector : public TransportWrapper{
public:
    TransportFaultInjector()
         :TransportWrapper() {
    }

    ~TransportFaultInjector() {
    }

    virtual void send(const char *data, size_t len)
    {
        if (m_interruptAtMessage == m_messageCount) {
            SE_LOG_DEBUG(NULL, NULL, "TransportFaultInjector: interrupt before sending message #%d", m_messageCount);
        }
        m_messageCount++;
        if (m_interruptAtMessage >= 0 &&
            m_messageCount > m_interruptAtMessage) {
            throw string("TransportFaultInjector: interrupt before send");
        }
    
        m_wrappedAgent->send(data, len);

        m_status = m_wrappedAgent->wait();
        
        if (m_interruptAtMessage == m_messageCount) {
            SE_LOG_DEBUG(NULL, NULL, "TransportFaultInjector: interrupt after receiving reply #%d", m_messageCount);
        }
        m_messageCount++;
        if (m_interruptAtMessage >= 0 &&
            m_messageCount > m_interruptAtMessage) {
            m_status = FAILED;
        }
    }

    virtual void getReply(const char *&data, size_t &len, std::string &contentType) {
        if (m_status == FAILED) {
            data = "";
            len = 0;
        } else {
            m_wrappedAgent->getReply(data, len, contentType);
        }
    }
};

/**
 * Swallow data at various points:
 * - between "client sent data" and "server receives data"
 * - after "server received data" and before "server sends reply"
 * - after "server has sent reply"
 *
 * The client deals with it by resending. This is similar to
 * TransportResendInjector and the ::Resend tests, but more thorough,
 * and stresses the HTTP server more (needs to deal with "reply not
 * delivered" error).
 *
 * Each send() increments the counter by three, so that 0 aborts
 * before the first message, 1 after sending it, and 2 after receiving
 * its reply.
 *
 * Swallowing data is implemented via the proxy.py script. This is
 * necessary because the wrapped agent has no API to trigger the second
 * error scenario. The wrapped agent is told to use a specific port
 * on localhost, with the base port passing message and reply through,
 * "base + 1" intercepting the message, etc.
 *
 * Because of the use of a proxy, this cannot be used to test servers
 * where a real proxy is needed.
 */
class TransportResendProxy : public TransportWrapper {
private:
    int port;
public:
    TransportResendProxy() : TransportWrapper() {
        const char *s = getenv("CLIENT_TEST_RESEND_PROXY");
        port = s ? atoi(s) : 0;
    }

    virtual int getResendFailureThreshold() { return 2; }

    virtual void send(const char *data, size_t len)
    {
        HTTPTransportAgent *agent = dynamic_cast<HTTPTransportAgent *>(m_wrappedAgent.get());
        CPPUNIT_ASSERT(agent);

        m_messageCount += 3;
        if (m_interruptAtMessage >= 0 &&
            m_interruptAtMessage < m_messageCount &&
            m_interruptAtMessage >= m_messageCount - 3) {
            int offset = m_interruptAtMessage - m_messageCount + 4;
            SE_LOG_DEBUG(NULL, NULL, "TransportResendProxy: interrupt %s",
                         offset == 1 ? "before sending message" :
                         offset == 2 ? "directly after sending message" :
                         "after receiving reply");
            agent->setProxy(StringPrintf("http://127.0.0.1:%d",
                                         offset + port));
        } else {
            agent->setProxy("");
        }
        agent->send(data, len);
        m_status = agent->wait();
    }

    virtual void getReply(const char *&data, size_t &len, std::string &contentType) {
        if (m_status == FAILED) {
            data = "";
            len = 0;
        } else {
            m_wrappedAgent->getReply(data, len, contentType);
        }
    }
};


/**
 * Emulates a user suspend just after receving response 
 * from server.
 */
class UserSuspendInjector : public TransportWrapper{
public:
    UserSuspendInjector()
         :TransportWrapper() {
    }

    ~UserSuspendInjector() {
    }

    virtual void send(const char *data, size_t len)
    {
        m_wrappedAgent->send(data, len);
        m_status = m_wrappedAgent->wait();
    }

    virtual void getReply(const char *&data, size_t &len, std::string &contentType) {
        if (m_status == FAILED) {
            data = "";
            len = 0;
        } else {
            if (m_interruptAtMessage == m_messageCount) {
                 SE_LOG_DEBUG(NULL, NULL, "UserSuspendInjector: user suspend after getting reply #%d", m_messageCount);
            }
            m_messageCount++;
            if (m_interruptAtMessage >= 0 &&
                    m_messageCount > m_interruptAtMessage) {
                m_options->m_isSuspended = true;
            }
            m_wrappedAgent->getReply(data, len, contentType);
        }
    }
};

/**
 * This function covers different error scenarios that can occur
 * during real synchronization. To pass, clients must either force a
 * slow synchronization after a failed synchronization or implement
 * the error handling described in the design guide (track server's
 * status for added/updated/deleted items and resend unacknowledged
 * changes).
 *
 * The items used during these tests are synthetic. They are
 * constructed so that normally a server should be able to handle
 * twinning during a slow sync correctly.
 *
 * Errors are injected into a synchronization by wrapping the normal
 * HTTP transport agent. The wrapper enumerates messages sent between
 * client and server (i.e., one message exchange increments the
 * counter by two), starting from zero. It "cuts" the connection before
 * sending out the next message to the server respectively after the 
 * server has replied, but before returning the reply to the client.
 * The first case simulates a lost message from the client to the server
 * and the second case a lost message from the server to the client.
 *
 * The expected result is the same as in an uninterrupted sync, which
 * is done once at the beginning.
 *
 * Each test goes through the following steps:
 * - client A and B reset local data store
 * - client A creates 3 new items, remembers LUIDs
 * - refresh-from-client A sync
 * - refresh-from-client B sync
 * - client B creates 3 different items, remembers LUIDs
 * - client B syncs
 * - client A syncs => A, B, server are in sync
 * - client A modifies his items (depends on test) and
 *   sends changes to server => server has changes for B
 * - client B modifies his items (depends on test)
 * - client B syncs, transport wrapper simulates lost message n
 * - client B syncs again, resuming synchronization if possible or
 *   slow sync otherwise (responsibility of the client!)
 * - client A syncs (not tested yet: A should be sent exactly the changes made by B)
 * - test that A and B contain same items
 * - test that A contains the same items as the uninterrupted reference run
 * - repeat the steps above ranging starting with lost message 0 until no
 *   message got lost
 *
 * Set the CLIENT_TEST_INTERRUPT_AT env variable to a message number
 * >= 0 to execute one uninterrupted run and then interrupt at that
 * message. Set to -1 to just do the uninterrupted run.
 */
void SyncTests::doInterruptResume(int changes, 
                  boost::shared_ptr<TransportWrapper> wrapper)
{
    int interruptAtMessage = -1;
    const char *t = getenv("CLIENT_TEST_INTERRUPT_AT");
    int requestedInterruptAt = t ? atoi(t) : -2;
    const char *s = getenv("CLIENT_TEST_INTERRUPT_SLEEP");
    int sleep_t = s ? atoi(s) : 0;
    size_t i;
    std::string refFileBase = getCurrentTest() + ".ref.";
    bool equal = true;
    bool resend = wrapper->getResendFailureThreshold() != -1;
    bool suspend = dynamic_cast <UserSuspendInjector *> (wrapper.get()) != NULL;
    bool interrupt = dynamic_cast <TransportFaultInjector *> (wrapper.get()) != NULL;

    // better be large enough for complete DevInf, 20000 is already a
    // bit small when running with many stores
    size_t maxMsgSize = 20000;
    size_t changedItemSize = (changes & BIG) ?
        5 * maxMsgSize / 2 : // large enough to be split over three messages
        0;

    // After running the uninterrupted sync, we remember the number
    // of sent messages. We never interrupt between sending our
    // own last message and receiving the servers last reply,
    // because the server is unable to detect that we didn't get
    // the reply. It will complete the session whereas the client
    // suspends, leading to an unexpected slow sync the next time.
    int maxMsgNum = 0;

    while (true) {
        char buffer[80];
        sprintf(buffer, "%d", interruptAtMessage);
        const char *prefix = interruptAtMessage == -1 ? "complete" : buffer;
        SyncPrefix prefixA(prefix, *this);
        SyncPrefix prefixB(prefix, *accessClientB);

        std::vector< std::list<std::string> > clientAluids;
        std::vector< std::list<std::string> > clientBluids;

        // create new items in client A and sync to server
        clientAluids.resize(sources.size());
        for (i = 0; i < sources.size(); i++) {
            sources[i].second->deleteAll(sources[i].second->createSourceA);
            clientAluids[i] =
                sources[i].second->insertManyItems(sources[i].second->createSourceA,
                                                   1, 3, 0);
        }
        doSync("fromA", SyncOptions(RefreshFromLocalMode()));

        // init client B and add its items to server and client A
        accessClientB->doSync("initB", SyncOptions(RefreshFromPeerMode()));
        clientBluids.resize(sources.size());
        for (i = 0; i < sources.size(); i++) {
            clientBluids[i] =
                accessClientB->sources[i].second->insertManyItems(accessClientB->sources[i].second->createSourceA,
                                                                  11, 3, 0);
        }
        accessClientB->doSync("fromB", SyncOptions(SYNC_TWO_WAY));
        doSync("updateA", SyncOptions(SYNC_TWO_WAY));

        // => client A, B and server in sync with a total of six items

        // make changes as requested on client A and sync to server
        for (i = 0; i < sources.size(); i++) {
            if (changes & SERVER_ADD) {
                sources[i].second->insertManyItems(sources[i].second->createSourceA,
                                                   4, 1, changedItemSize);
            }
            if (changes & SERVER_REMOVE) {
                // remove second item
                removeItem(sources[i].second->createSourceA,
                           *(++clientAluids[i].begin()));
            }
            if (changes & SERVER_UPDATE) {
                // update third item
                updateItem(sources[i].second->createSourceA,
                           sources[i].second->config,
                           *(++ ++clientAluids[i].begin()),
                           sources[i].second->createItem(3, "updated", changedItemSize).c_str());
                                              
            }
        }

        // send using the same mode as in the interrupted sync with client B
        if (changes & (SERVER_ADD|SERVER_REMOVE|SERVER_UPDATE)) {
            doSync("changesFromA", SyncOptions(SYNC_TWO_WAY).setMaxMsgSize(maxMsgSize));
        }

        // make changes as requested on client B
        for (i = 0; i < sources.size(); i++) {
            if (changes & CLIENT_ADD) {
                accessClientB->sources[i].second->insertManyItems(accessClientB->sources[i].second->createSourceA,
                                                                  14, 1, changedItemSize);
            }
            if (changes & CLIENT_REMOVE) {
                // remove second item
                removeItem(accessClientB->sources[i].second->createSourceA,
                           *(++clientBluids[i].begin()));
            }
            if (changes & CLIENT_UPDATE) {
                // update third item
                updateItem(accessClientB->sources[i].second->createSourceA,
                           accessClientB->sources[i].second->config,
                           *(++ ++clientBluids[i].begin()),
                           accessClientB->sources[i].second->createItem(13, "updated", changedItemSize).c_str());
            }
        }

        // Now do an interrupted sync between B and server.
        // The explicit delete of the TransportAgent is suppressed
        // by overloading the delete operator.
        int wasInterrupted;
        {
            CheckSyncReport check(-1, -1, -1, -1, -1, -1, false);
            if (resend && interruptAtMessage > wrapper->getResendFailureThreshold()) {
                // resend tests must succeed, except for the first
                // message in the session, which is not resent
                check.mustSucceed = true;
            }
            SyncOptions options(SYNC_TWO_WAY, check);
            options.setTransportAgent(wrapper);
            options.setMaxMsgSize(maxMsgSize);
            // disable resending completely or shorten the resend
            // interval to speed up testing
            options.setRetryInterval(resend ? 10 : 0);
            wrapper->setInterruptAtMessage(interruptAtMessage);
            accessClientB->doSync("changesFromB", options);
            wasInterrupted = interruptAtMessage != -1 &&
                wrapper->getMessageCount() <= interruptAtMessage;
            if (!maxMsgNum) {
                maxMsgNum = wrapper->getMessageCount();
            }
            wrapper->rewind();
        }

        if (interruptAtMessage != -1) {
            if (wasInterrupted) {
                // uninterrupted sync, done
                break;
            }

            // continue, wait until server timeout
            if(sleep_t) 
                sleep (sleep_t);

            // no need for resend tests, unless they were interrupted at the first message
            if (!resend || interruptAtMessage <= wrapper->getResendFailureThreshold()) {
                SyncReport report;
                accessClientB->doSync("retryB",
                                      SyncOptions(SYNC_TWO_WAY,
                                                  CheckSyncReport().setMode(SYNC_TWO_WAY).setReport(&report)));
                // Suspending at first and last message doesn't need a
                // resume, everything else does. When multiple sources
                // are involved, some may suspend, some may not, so we
                // cannot check.
                if (suspend &&
                    interruptAtMessage != 0 &&
                    interruptAtMessage + 1 != maxMsgNum &&
                    report.size() == 1) {
                    BOOST_FOREACH(const SyncReport::SourceReport_t &sourceReport, report) {
                        CPPUNIT_ASSERT(sourceReport.second.isResumeSync());
                    }
                }
            }
        }

        // copy changes to client A
        doSync("toA", SyncOptions(SYNC_TWO_WAY));

        // compare client A and B
        if (interruptAtMessage != -1 &&
            !compareDatabases(refFileBase.c_str(), false)) {
            equal = false;
            std::cerr << "====> comparison of client B against reference file(s) failed after interrupting at message #" <<
                interruptAtMessage << std::endl;
            std::cerr.flush();
        }
        if (!compareDatabases(NULL, false)) {
            equal = false;
            std::cerr << "====> comparison of client A and B failed after interrupting at message #" <<
                interruptAtMessage << std::endl;
            std::cerr.flush();
        }

        // save reference files from uninterrupted run?
        if (interruptAtMessage == -1) {
            for (source_it it = sources.begin();
                 it != sources.end();
                 ++it) {
                std::string refFile = refFileBase;
                refFile += it->second->config.m_sourceName;
                refFile += ".dat";
                simplifyFilename(refFile);
                TestingSyncSourcePtr source;
                SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceA()));
                SOURCE_ASSERT_EQUAL(source.get(), 0, it->second->config.m_dump(client, *source.get(), refFile.c_str()));
                CPPUNIT_ASSERT_NO_THROW(source.reset());
            }
        }

        // pick next iteration
        if (requestedInterruptAt == -1) {
            // user requested to stop after first iteration
            break;
        } else if (requestedInterruptAt >= 0) {
            // only do one interrupted run of the test
            if (requestedInterruptAt == interruptAtMessage) {
                break;
            } else {
                interruptAtMessage = requestedInterruptAt;
            }
        } else {
            // interrupt one message later than before
            interruptAtMessage++;
            if (interrupt &&
                interruptAtMessage + 1 >= maxMsgNum) {
                // Don't interrupt before the server's last reply,
                // because then the server thinks we completed the
                // session when we think we didn't, which leads to a
                // slow sync. Testing that is better done with a
                // specific test.
                break;
            }
            if (interruptAtMessage >= maxMsgNum) {
                // next run would not interrupt at all, stop now
                break;
            }
        }
    }

    CPPUNIT_ASSERT(equal);
}

void SyncTests::testInterruptResumeClientAdd()
{
    doInterruptResume(CLIENT_ADD, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector()));
}

void SyncTests::testInterruptResumeClientRemove()
{
    doInterruptResume(CLIENT_REMOVE, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector()));
}

void SyncTests::testInterruptResumeClientUpdate()
{
    doInterruptResume(CLIENT_UPDATE, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector()));
}

void SyncTests::testInterruptResumeServerAdd()
{
    doInterruptResume(SERVER_ADD, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector()));
}

void SyncTests::testInterruptResumeServerRemove()
{
    doInterruptResume(SERVER_REMOVE, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector()));
}

void SyncTests::testInterruptResumeServerUpdate()
{
    doInterruptResume(SERVER_UPDATE, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector()));
}

void SyncTests::testInterruptResumeClientAddBig()
{
    doInterruptResume(CLIENT_ADD|BIG, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector()));
}

void SyncTests::testInterruptResumeClientUpdateBig()
{
    doInterruptResume(CLIENT_UPDATE|BIG, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector()));
}

void SyncTests::testInterruptResumeServerAddBig()
{
    doInterruptResume(SERVER_ADD|BIG, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector()));
}

void SyncTests::testInterruptResumeServerUpdateBig()
{
    doInterruptResume(SERVER_UPDATE|BIG, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector()));
}

void SyncTests::testInterruptResumeFull()
{
    doInterruptResume(CLIENT_ADD|CLIENT_REMOVE|CLIENT_UPDATE|
                      SERVER_ADD|SERVER_REMOVE|SERVER_UPDATE, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector()));
}

void SyncTests::testUserSuspendClientAdd()
{
    doInterruptResume(CLIENT_ADD, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector()));
}

void SyncTests::testUserSuspendClientRemove()
{
    doInterruptResume(CLIENT_REMOVE, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector()));
}

void SyncTests::testUserSuspendClientUpdate()
{
    doInterruptResume(CLIENT_UPDATE, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector()));
}

void SyncTests::testUserSuspendServerAdd()
{
    doInterruptResume(SERVER_ADD, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector()));
}

void SyncTests::testUserSuspendServerRemove()
{
    doInterruptResume(SERVER_REMOVE, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector()));
}

void SyncTests::testUserSuspendServerUpdate()
{
    doInterruptResume(SERVER_UPDATE, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector()));
}

void SyncTests::testUserSuspendClientAddBig()
{
    doInterruptResume(CLIENT_ADD|BIG, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector()));
}

void SyncTests::testUserSuspendClientUpdateBig()
{
    doInterruptResume(CLIENT_UPDATE|BIG, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector()));
}

void SyncTests::testUserSuspendServerAddBig()
{
    doInterruptResume(SERVER_ADD|BIG, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector()));
}

void SyncTests::testUserSuspendServerUpdateBig()
{
    doInterruptResume(SERVER_UPDATE|BIG, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector()));
}

void SyncTests::testUserSuspendFull()
{
    doInterruptResume(CLIENT_ADD|CLIENT_REMOVE|CLIENT_UPDATE|
                      SERVER_ADD|SERVER_REMOVE|SERVER_UPDATE, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector()));
}

void SyncTests::testResendClientAdd()
{
    doInterruptResume(CLIENT_ADD, boost::shared_ptr<TransportWrapper> (new TransportResendInjector()));
}

void SyncTests::testResendClientRemove()
{
    doInterruptResume(CLIENT_REMOVE, boost::shared_ptr<TransportWrapper> (new TransportResendInjector()));
}

void SyncTests::testResendClientUpdate()
{
    doInterruptResume(CLIENT_UPDATE, boost::shared_ptr<TransportWrapper> (new TransportResendInjector()));
}

void SyncTests::testResendServerAdd()
{
    doInterruptResume(SERVER_ADD, boost::shared_ptr<TransportWrapper> (new TransportResendInjector()));
}

void SyncTests::testResendServerRemove()
{
    doInterruptResume(SERVER_REMOVE, boost::shared_ptr<TransportWrapper> (new TransportResendInjector()));
}

void SyncTests::testResendServerUpdate()
{
    doInterruptResume(SERVER_UPDATE, boost::shared_ptr<TransportWrapper> (new TransportResendInjector()));
}

void SyncTests::testResendFull()
{
    doInterruptResume(CLIENT_ADD|CLIENT_REMOVE|CLIENT_UPDATE|
                      SERVER_ADD|SERVER_REMOVE|SERVER_UPDATE, 
                      boost::shared_ptr<TransportWrapper> (new TransportResendInjector()));
}

void SyncTests::testResendProxyClientAdd()
{
    doInterruptResume(CLIENT_ADD, boost::shared_ptr<TransportWrapper> (new TransportResendProxy()));
}

void SyncTests::testResendProxyClientRemove()
{
    doInterruptResume(CLIENT_REMOVE, boost::shared_ptr<TransportWrapper> (new TransportResendProxy()));
}

void SyncTests::testResendProxyClientUpdate()
{
    doInterruptResume(CLIENT_UPDATE, boost::shared_ptr<TransportWrapper> (new TransportResendProxy()));
}

void SyncTests::testResendProxyServerAdd()
{
    doInterruptResume(SERVER_ADD, boost::shared_ptr<TransportWrapper> (new TransportResendProxy()));
}

void SyncTests::testResendProxyServerRemove()
{
    doInterruptResume(SERVER_REMOVE, boost::shared_ptr<TransportWrapper> (new TransportResendProxy()));
}

void SyncTests::testResendProxyServerUpdate()
{
    doInterruptResume(SERVER_UPDATE, boost::shared_ptr<TransportWrapper> (new TransportResendProxy()));
}

void SyncTests::testResendProxyFull()
{
    doInterruptResume(CLIENT_ADD|CLIENT_REMOVE|CLIENT_UPDATE|
                      SERVER_ADD|SERVER_REMOVE|SERVER_UPDATE, 
                      boost::shared_ptr<TransportWrapper> (new TransportResendProxy()));
}

static bool setDeadSyncURL(SyncContext &context,
                           SyncOptions &options,
                           int port,
                           bool *skipped)
{
    vector<string> urls = context.getSyncURL();
    string url;
    if (urls.size() == 1) {
        url = urls.front();
    }

    // use IPv4 localhost address, that's what we listen on
    string fakeURL = StringPrintf("http://127.0.0.1:%d/foobar", port);

    if (boost::starts_with(url, "http")) {
        context.setSyncURL(fakeURL, true);
        context.setSyncUsername("foo", true);
        context.setSyncPassword("bar", true);
        return false;
    } else if (boost::starts_with(url, "local://")) {
        FullProps props = context.getConfigProps();
        string target = url.substr(strlen("local://"));
        props[target].m_syncProps["syncURL"] = fakeURL;
        props[target].m_syncProps["retryDuration"] = "10";
        props[target].m_syncProps["retryInterval"] = "10";
        context.setConfigProps(props);
        return false;
    } else {
        // cannot run test, tell parent
        *skipped = true;
        return true;
    }
}

void SyncTests::testTimeout()
{
    // Create a dead listening socket, then run a sync with a sync URL
    // which points towards localhost at that port. Do this with no
    // message resending and a very short overall timeout. The
    // expectation is that the transmission timeout strikes.
    time_t start = time(NULL);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CPPUNIT_ASSERT(fd != -1);
    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    int res = bind(fd, (sockaddr *)&servaddr, sizeof(servaddr));
    CPPUNIT_ASSERT(res == 0);
    socklen_t len = sizeof(servaddr);
    res = getsockname(fd, (sockaddr *)&servaddr, &len);
    CPPUNIT_ASSERT(res == 0);
    res = listen(fd, 10);
    CPPUNIT_ASSERT(res == 0);
    bool skipped = false;
    SyncReport report;
    doSync("timeout",
           SyncOptions(SYNC_SLOW,
                       CheckSyncReport(-1, -1, -1, -1, -1, -1,
                                       false).setReport(&report))
           .setPrepareCallback(boost::bind(setDeadSyncURL, _1, _2, ntohs(servaddr.sin_port), &skipped))
           .setRetryDuration(20)
           .setRetryInterval(20));
    time_t end = time(NULL);
    close(fd);
    if (!skipped) {
        CPPUNIT_ASSERT_EQUAL(STATUS_TRANSPORT_FAILURE, report.getStatus());
        CPPUNIT_ASSERT(end - start >= 19);
        CPPUNIT_ASSERT(end - start < 40); // needs to be sufficiently larger than 20s timeout
                                          // because under valgrind the startup time is considerable
    }
}

void SyncTests::doSync(const SyncOptions &options)
{
    int res = 0;
    static int syncCounter = 0;
    static std::string lastTest;
    std::stringstream logstream;

    // reset counter when switching tests
    if (lastTest != getCurrentTest()) {
        syncCounter = 0;
        lastTest = getCurrentTest();
    }

    std::string prefix;
    prefix.reserve(80);
    for (std::list<std::string>::iterator it = logPrefixes.begin();
         it != logPrefixes.end();
         ++it) {
        prefix += ".";
        prefix += *it;
    }
    if (!prefix.empty()) {
        printf(" %s", prefix.c_str() + 1);
        fflush(stdout);
    }

    logstream /* << std::setw(4) << std::setfill('0') << syncCounter << "_" */ << getCurrentTest()
                 << prefix
                 << ".client." << (accessClientB ? "A" : "B");
    std::string logname = logstream.str();
    simplifyFilename(logname);
    syncCounter++;

    SE_LOG_DEBUG(NULL, NULL, "%d. starting %s with sync mode %s",
                 syncCounter, logname.c_str(), PrettyPrintSyncMode(options.m_syncMode).c_str());

    try {
        res = client.doSync(sourceArray,
                            logname,
                            options);

        postSync(res, logname);
    } catch (CppUnit::Exception &ex) {
        res = 1;
        postSync(res, logname);

        // report the original exception without altering the source line
        throw;
    } catch (...) {
        res = 1;
        postSync(res, logname);

        // this logs the original exception using CPPUnit mechanisms,
        // with current line as source
        CPPUNIT_ASSERT_NO_THROW(throw);
    }
}

void SyncTests::postSync(int res, const std::string &logname)
{
    char *log = getenv("CLIENT_TEST_LOG");

    client.postSync(res, logname);
    if (log &&
        !access(log, F_OK)) {
        // give server time to finish writing its logs:
        // more time after a failure
        sleep(res ? 5 : 1);
        if (system(StringPrintf("cp -a '%s' '%s/server-log'", log, logname.c_str()).c_str()) < 0) {
            SE_LOG_WARNING(NULL, NULL, "Unable too copy server log: %s", log);
        }
        rm_r(log);
    }
}

/** generates tests on demand based on what the client supports */
class ClientTestFactory : public CppUnit::TestFactory {
public:
    ClientTestFactory(ClientTest &c) :
        client(c) {}

    virtual CppUnit::Test *makeTest() {
        int source;
        CppUnit::TestSuite *alltests = new CppUnit::TestSuite("Client");
        CppUnit::TestSuite *tests;

        // create local source tests
        tests = new CppUnit::TestSuite(alltests->getName() + "::Source");
        for (source=0; source < client.getNumLocalSources(); source++) {
            ClientTest::Config config;
            client.getLocalSourceConfig(source, config);
            if (!config.m_sourceName.empty()) {
                LocalTests *sourcetests =
                    client.createLocalTests(tests->getName() + "::" + config.m_sourceName, source, config);
                sourcetests->addTests();
                tests->addTest(FilterTest(sourcetests));
            }
        }
        alltests->addTest(FilterTest(tests));
        tests = 0;

        // create sync tests with just one source
        tests = new CppUnit::TestSuite(alltests->getName() + "::Sync");
        for (source=0; source < client.getNumSyncSources(); source++) {
            ClientTest::Config config;
            client.getSyncSourceConfig(source, config);
            if (!config.m_sourceName.empty()) {
                std::vector<int> sources;
                sources.push_back(source);
                SyncTests *synctests =
                    client.createSyncTests(tests->getName() + "::" + config.m_sourceName, sources);
                synctests->addTests(source == 0);
                tests->addTest(FilterTest(synctests));
            }
        }

        // create sync tests with all sources enabled, unless we only have one:
        // that would be identical to the test above
        std::vector<int> sources;
        std::string name, name_reversed;
        for (source=0; source < client.getNumSyncSources(); source++) {
            ClientTest::Config config;
            client.getSyncSourceConfig(source, config);
            if (!config.m_sourceName.empty()) {
                sources.push_back(source);
                if (name.size() > 0) {
                    name += "_";
                    name_reversed = std::string("_") + name_reversed;
                }
                name += config.m_sourceName;
                name_reversed = config.m_sourceName + name_reversed;
            }
        }
        if (sources.size() > 1) {
            SyncTests *synctests =
                client.createSyncTests(tests->getName() + "::" + name, sources);
            synctests->addTests();
            tests->addTest(FilterTest(synctests));
            synctests = 0;

            if (getenv("CLIENT_TEST_REVERSE_SOURCES")) {
                // now also in reversed order - who knows, it might make a difference;
                // typically it just makes the whole run slower, so not enabled
                // by default
                std::reverse(sources.begin(), sources.end());
                synctests =
                    client.createSyncTests(tests->getName() + "::" + name_reversed, sources);
                synctests->addTests();
                tests->addTest(FilterTest(synctests));
                synctests = 0;
            }
        }

        alltests->addTest(FilterTest(tests));
        tests = 0;

        return alltests;
    }

private:
    ClientTest &client;
};

void ClientTest::registerTests()
{
    factory = (void *)new ClientTestFactory(*this);
    CppUnit::TestFactoryRegistry::getRegistry().registerFactory((CppUnit::TestFactory *)factory);
}

ClientTest::ClientTest(int serverSleepSec, const std::string &serverLog) :
    serverSleepSeconds(serverSleepSec),
    serverLogFileName(serverLog),
    factory(NULL)
{
}

ClientTest::~ClientTest()
{
    if(factory) {
        CppUnit::TestFactoryRegistry::getRegistry().unregisterFactory((CppUnit::TestFactory *)factory);
        delete (CppUnit::TestFactory *)factory;
        factory = 0;
    }
}

void ClientTest::registerCleanup(Cleanup_t cleanup)
{
    cleanupSet.insert(cleanup);
}

void ClientTest::shutdown()
{
    BOOST_FOREACH(Cleanup_t cleanup, cleanupSet) {
        cleanup();
    }
}

LocalTests *ClientTest::createLocalTests(const std::string &name, int sourceParam, ClientTest::Config &co)
{
    return new LocalTests(name, *this, sourceParam, co);
}

SyncTests *ClientTest::createSyncTests(const std::string &name, std::vector<int> sourceIndices, bool isClientA)
{
    return new SyncTests(name, *this, sourceIndices, isClientA);
}

int ClientTest::dump(ClientTest &client, TestingSyncSource &source, const std::string &file)
{
    BackupReport report;
    boost::shared_ptr<ConfigNode> node(new VolatileConfigNode);

    rm_r(file);
    mkdir_p(file);
    CPPUNIT_ASSERT(source.getOperations().m_backupData);
    source.getOperations().m_backupData(SyncSource::Operations::ConstBackupInfo(),
                                        SyncSource::Operations::BackupInfo(SyncSource::Operations::BackupInfo::BACKUP_OTHER, file, node),
                                        report);
    return 0;
}

void ClientTest::getItems(const std::string &file, list<string> &items, std::string &testcases)
{
    items.clear();

    // import the file, trying a .tem file (base file plus patch)
    // first
    std::ifstream input;
    string server = currentServer();
    testcases = file + '.' + server +".tem";
    input.open(testcases.c_str());

    if (input.fail()) {
        // try server-specific file (like eds_event.ics.local)
        testcases = file + '.' + server;
        input.open(testcases.c_str());
    }

    if (input.fail()) {
        // try base file
        testcases = file;
        input.open(testcases.c_str());
    }
    CPPUNIT_ASSERT(!input.bad());
    CPPUNIT_ASSERT(input.is_open());
    std::string data, line;
    while (input) {
        bool wasend = false;
        do {
            getline(input, line);
            CPPUNIT_ASSERT(!input.bad());
            // empty lines directly after line which starts with END mark end of record;
            // check for END necessary becayse vCard 2.1 ENCODING=BASE64 may have empty lines in body of VCARD!
            if ((line != "\r" && line.size() > 0) || !wasend) {
                data += line;
                data += "\n";
            } else {
                if (!data.empty()) {
                    items.push_back(data);
                }
                data = "";
            }
            wasend = !line.compare(0, 4, "END:");
        } while(!input.eof());
    }
    if (!data.empty()) {
        items.push_back(data);
    }
}

std::string ClientTest::import(ClientTest &client, TestingSyncSource &source, const ClientTestConfig &config,
                               const std::string &file, std::string &realfile)
{
    list<string> items;
    getItems(file, items, realfile);
    std::string failures;
    BOOST_FOREACH(string &data, items) {
        try {
            importItem(&source, config, data);
        } catch (...) {
            std::string explanation;
            Exception::handle(explanation);
            failures += "Failed to import:\n";
            failures += data;
            failures += "\n";
            failures += explanation;
            failures += "\n";
        }
    }
    return failures;
}

bool ClientTest::compare(ClientTest &client, const std::string &fileA, const std::string &fileB)
{
    std::string cmdstr = std::string("env PATH=.:$PATH synccompare ") + fileA + " " + fileB;
    setenv("CLIENT_TEST_HEADER", "\n\n", 1);
    setenv("CLIENT_TEST_LEFT_NAME", fileA.c_str(), 1);
    setenv("CLIENT_TEST_RIGHT_NAME", fileB.c_str(), 1);
    setenv("CLIENT_TEST_REMOVED", "only in left file", 1);
    setenv("CLIENT_TEST_ADDED", "only in right file", 1);
    const char* compareLog = getenv("CLIENT_TEST_COMPARE_LOG");
    if(compareLog && strlen(compareLog))
    {
       string tmpfile = "____compare.log";
       cmdstr =string("bash -c 'set -o pipefail;") + cmdstr;
       cmdstr += " 2>&1|tee " +tmpfile+"'";
    }
    bool success = system(cmdstr.c_str()) == 0;
    if (!success) {
        printf("failed: env CLIENT_TEST_SERVER=%s PATH=.:$PATH synccompare %s %s\n",
               currentServer().c_str(),
               fileA.c_str(), fileB.c_str());
    }
    return success;
}

void ClientTest::update(std::string &item)
{
    const static char *props[] = {
        "\nSUMMARY",
        "\nNOTE",
        NULL
    };

    for (const char **prop = props; *prop; prop++) {
        size_t pos;
        pos = item.find(*prop);
        if (pos != item.npos) {
            // Modify existing property. Fast-forward to : (works as
            // long as colon is not in parameters).
            pos = item.find(':', pos);
        }
        if (pos != item.npos) {
            item.insert(pos + 1, "MOD-");
        } else if (!strcmp(*prop, "\nNOTE") && (pos = item.find("END:VCARD")) != item.npos) {
            // add property, but only if it is allowed in the item
            item.insert(pos, "NOTE:MOD\n");
        }
    }
}

void ClientTest::postSync(int res, const std::string &logname)
{
#ifdef WIN32
    Sleep(serverSleepSeconds * 1000);
#else
    sleep(serverSleepSeconds);

    // make a copy of the server's log (if found), then truncate it
    if (serverLogFileName.size()) {
        int fd = open(serverLogFileName.c_str(), O_RDWR);

        if (fd >= 0) {
            std::string cmd = std::string("cp ") + serverLogFileName + " " + logname + ".server.log";
            if (system(cmd.c_str())) {
                fprintf(stdout, "copying log file failed: %s\n", cmd.c_str());
            }
            if (ftruncate(fd, 0)) {
                perror("truncating log file");
            }
        } else {
            perror(serverLogFileName.c_str());
        }
    }
#endif
}

static string mangleGeneric(const std::string &data, bool update)
{
    std::string item = data;
    if (update) {
        boost::replace_first(item, "NOTE:", "NOTE:U ");
    }
    return item;
}

static string mangleICalendar20(const std::string &data, bool update)
{
    std::string item = data;

    if (update) {
        if (item.find("BEGIN:VJOURNAL") != item.npos) {
            // Need to modify first line of description and summary
            // consistently for a note because in plain text
            // representation, these lines are expected to be
            // identical.
            boost::replace_first(item, "SUMMARY:", "SUMMARY:U ");
        }
        boost::replace_first(item, "DESCRIPTION:", "DESCRIPTION:U ");
    }

    if (getenv("CLIENT_TEST_NO_UID")) {
        boost::replace_all(item, "UID:1234567890!@#$%^&*()<>@dummy\n", "");
    } else if (getenv("CLIENT_TEST_SIMPLE_UID")) {
        boost::replace_all(item, "UID:1234567890!@#$%^&*()<>@dummy", "UID:1234567890@dummy");
    }

    if (getenv("CLIENT_TEST_UNIQUE_UID")) {
        // Making UID unique per test to avoid issues
        // when the source already holds older copies.
        // Might still be an issue in real life?!
        static time_t start;
        static std::string test;
        if (test != getCurrentTest()) {
            start = time(NULL);
            test = getCurrentTest();
        }
        std::string unique = StringPrintf("UID:UNIQUE-UID-%llu-", (long long unsigned)start);
        boost::replace_all(item, "UID:", unique);
    } else if (getenv("CLIENT_TEST_LONG_UID")) {
        boost::replace_all(item, "UID:", "UID:this-is-a-ridiculously-long-uid-");
    }

    size_t offset = item.find("\nLAST-MODIFIED:");
    static const size_t len = strlen("\nLAST-MODIFIED:20100131T235959Z");
    if (offset != item.npos) {
        // Special semantic for iCalendar 2.0: LAST-MODIFIED should be
        // incremented in updated items. Emulate that by inserting the
        // current time.
        time_t now = time(NULL);
        struct tm tm;
        gmtime_r(&now, &tm);
        std::string mod = StringPrintf("\nLAST-MODIFIED:%04d%02d%02dT%02d%02d%02dZ",
                                       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                                       tm.tm_hour, tm.tm_min, tm.tm_sec);
        item.replace(offset, len, mod);
    }

    const static string sequence("\nSEQUENCE:XXX");
    offset = item.find(sequence);
    if (offset != item.npos) {
        if (getenv("CLIENT_TEST_INCREASE_SEQUENCE")) {
            // Increment sequence number in steps of 100 to ensure that our
            // new item is considered more recent than any corresponding
            // item in the source. Some storages (Google CalDAV) check that.
            static int counter = 100;
            item.replace(offset, sequence.size(), StringPrintf("\nSEQUENCE:%d", counter));
            counter += 100;
        } else {
            item.replace(offset, sequence.size(), "\nSEQUENCE:1");
        }
    }

    return item;
}

void ClientTest::getTestData(const char *type, Config &config)
{
    config = Config();
    char *env = getenv("CLIENT_TEST_RETRY");
    config.m_retrySync = (env && !strcmp (env, "t")) ?true :false;
    env = getenv("CLIENT_TEST_RESEND");
    config.m_resendSync = (env && !strcmp (env, "t")) ?true :false;
    env = getenv("CLIENT_TEST_SUSPEND");
    config.m_suspendSync = (env && !strcmp (env, "t")) ?true :false;
    config.m_sourceKnowsItemSemantic = true;
    config.m_linkedItemsRelaxedSemantic = true;
    config.m_itemType = "";
    config.m_import = import;
    config.m_dump = dump;
    config.m_compare = compare;
    // Sync::*::testExtensions not enabled by default.
    config.m_update = 0;
    config.m_genericUpdate = update;

    // redirect requests for "eds_event" towards "eds_event_noutc"?
    bool noutc = false;
    env = getenv ("CLIENT_TEST_NOUTC");
    if (env && !strcmp (env, "t")) {
        noutc = true;
    }

    config.m_mangleItem = mangleGeneric;

    if (!strcmp(type, "eds_contact")) {
        config.m_sourceName = "eds_contact";
        config.m_sourceNameServerTemplate = "addressbook";
        config.m_uri = "card3"; // ScheduleWorld
        config.m_type = "text/vcard";
        config.m_insertItem =
            "BEGIN:VCARD\n"
            "VERSION:3.0\n"
            "TITLE:tester\n"
            "FN:John Doe\n"
            "N:Doe;John;;;\n"
            "TEL;TYPE=WORK;TYPE=VOICE:business 1\n"
            "X-EVOLUTION-FILE-AS:Doe\\, John\n"
            "X-MOZILLA-HTML:FALSE\n"
            "END:VCARD\n";
        config.m_updateItem =
            "BEGIN:VCARD\n"
            "VERSION:3.0\n"
            "TITLE:tester\n"
            "FN:Joan Doe\n"
            "N:Doe;Joan;;;\n"
            "X-EVOLUTION-FILE-AS:Doe\\, Joan\n"
            "TEL;TYPE=WORK;TYPE=VOICE:business 2\n"
            "BDAY:2006-01-08\n"
            "X-MOZILLA-HTML:TRUE\n"
            "END:VCARD\n";
        /* adds a second phone number: */
        config.m_complexUpdateItem =
            "BEGIN:VCARD\n"
            "VERSION:3.0\n"
            "TITLE:tester\n"
            "FN:Joan Doe\n"
            "N:Doe;Joan;;;\n"
            "X-EVOLUTION-FILE-AS:Doe\\, Joan\n"
            "TEL;TYPE=WORK;TYPE=VOICE:business 1\n"
            "TEL;TYPE=HOME;TYPE=VOICE:home 2\n"
            "BDAY:2006-01-08\n"
            "X-MOZILLA-HTML:TRUE\n"
            "END:VCARD\n";
        /* add a telephone number, email and X-AIM to initial item */
        config.m_mergeItem1 =
            "BEGIN:VCARD\n"
            "VERSION:3.0\n"
            "TITLE:tester\n"
            "FN:John Doe\n"
            "N:Doe;John;;;\n"
            "X-EVOLUTION-FILE-AS:Doe\\, John\n"
            "X-MOZILLA-HTML:FALSE\n"
            "TEL;TYPE=WORK;TYPE=VOICE:business 1\n"
            "EMAIL:john.doe@work.com\n"
            "X-AIM:AIM JOHN\n"
            "END:VCARD\n";
        config.m_mergeItem2 =
            "BEGIN:VCARD\n"
            "VERSION:3.0\n"
            "TITLE:developer\n"
            "FN:John Doe\n"
            "N:Doe;John;;;\n"
            "TEL;TYPE=WORK;TYPE=VOICE:123456\n"
            "X-EVOLUTION-FILE-AS:Doe\\, John\n"
            "X-MOZILLA-HTML:TRUE\n"
            "BDAY:2006-01-08\n"
            "END:VCARD\n";
        // use NOTE and N to make the item unique
        config.m_templateItem =
            "BEGIN:VCARD\n"
            "VERSION:3.0\n"
            "TITLE:tester\n"
            "N:Doe;<<UNIQUE>>;<<REVISION>>;;\n"
            "FN:<<UNIQUE>> Doe\n"
            "TEL;TYPE=WORK;TYPE=VOICE:business 1\n"
            "X-EVOLUTION-FILE-AS:Doe\\, <<UNIQUE>>\n"
            "X-MOZILLA-HTML:FALSE\n"
            "NOTE:<<REVISION>>\n"
            "END:VCARD\n";  
        config.m_uniqueProperties = "";
        config.m_sizeProperty = "NOTE";
        config.m_testcases = "testcases/eds_contact.vcf";
    } else if (!strcmp(type, "eds_event") && !noutc) {
        config.m_sourceName = "eds_event";
        config.m_sourceNameServerTemplate = "calendar";
        config.m_uri = "cal2"; // ScheduleWorld
        config.m_type = "text/x-vcalendar";
        config.m_mangleItem = mangleICalendar20;
        config.m_insertItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VEVENT\n"
            "SUMMARY:phone meeting\n"
            "DTEND:20060406T163000Z\n"
            "DTSTART:20060406T160000Z\n"
            "UID:1234567890!@#$%^&*()<>@dummy\n"
            "DTSTAMP:20060406T211449Z\n"
            "LAST-MODIFIED:20060409T213201Z\n"
            "CREATED:20060409T213201Z\n"
            "LOCATION:my office\n"
            "DESCRIPTION:let's talk<<REVISION>>\n"
            "CLASS:PUBLIC\n"
            "TRANSP:OPAQUE\n"
            "SEQUENCE:XXX\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        config.m_updateItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VEVENT\n"
            "SUMMARY:meeting on site\n"
            "DTEND:20060406T163000Z\n"
            "DTSTART:20060406T160000Z\n"
            "UID:1234567890!@#$%^&*()<>@dummy\n"
            "DTSTAMP:20060406T211449Z\n"
            "LAST-MODIFIED:20060409T213201Z\n"
            "CREATED:20060409T213201Z\n"
            "SEQUENCE:XXX\n"
            "LOCATION:big meeting room\n"
            "DESCRIPTION:nice to see you\n"
            "CLASS:PUBLIC\n"
            "TRANSP:OPAQUE\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        /* change location and description of insertItem in testMerge(), add alarm */
        config.m_mergeItem1 =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VEVENT\n"
            "SUMMARY:phone meeting\n"
            "DTEND:20060406T163000Z\n"
            "DTSTART:20060406T160000Z\n"
            "UID:1234567890!@#$%^&*()<>@dummy\n"
            "DTSTAMP:20060406T211449Z\n"
            "LAST-MODIFIED:20060409T213201Z\n"
            "CREATED:20060409T213201Z\n"
            "SEQUENCE:XXX\n"
            "LOCATION:calling from home\n"
            "DESCRIPTION:let's talk\n"
            "CLASS:PUBLIC\n"
            "TRANSP:OPAQUE\n"
            "BEGIN:VALARM\n"
            "DESCRIPTION:alarm\n"
            "ACTION:DISPLAY\n"
            "TRIGGER;VALUE=DURATION;RELATED=START:-PT15M\n"
            "END:VALARM\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        /* change location to something else, add category */
        config.m_mergeItem2 =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VEVENT\n"
            "SUMMARY:phone meeting\n"
            "DTEND:20060406T163000Z\n"
            "DTSTART:20060406T160000Z\n"
            "UID:1234567890!@#$%^&*()<>@dummy\n"
            "DTSTAMP:20060406T211449Z\n"
            "LAST-MODIFIED:20060409T213201Z\n"
            "CREATED:20060409T213201Z\n"
            "SEQUENCE:XXX\n"
            "LOCATION:my office\n"
            "CATEGORIES:WORK\n"
            "DESCRIPTION:what the heck\\, let's even shout a bit\n"
            "CLASS:PUBLIC\n"
            "TRANSP:OPAQUE\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";

        // Servers have very different understandings of how
	// recurrence interacts with time zones and RRULE.
	// Must use different test cases for some servers to
	// avoid having the linkedItems test cases fail
	// because of that.
	std::string server = currentServer();
	// default: time zones + UNTIL in UTC, with VALARM
        config.m_linkedItems.resize(1);
        config.m_linkedItems[0].resize(2);
        config.m_linkedItems[0][0] =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VTIMEZONE\n"
            "TZID:/softwarestudio.org/Olson_20011030_5/Europe/Berlin\n"
            "X-LIC-LOCATION:Europe/Berlin\n"
            "BEGIN:DAYLIGHT\n"
            "TZOFFSETFROM:+0100\n"
            "TZOFFSETTO:+0200\n"
            "TZNAME:CEST\n"
            "DTSTART:19700329T020000\n"
            "RRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=-1SU;BYMONTH=3\n"
            "END:DAYLIGHT\n"
            "BEGIN:STANDARD\n"
            "TZOFFSETFROM:+0200\n"
            "TZOFFSETTO:+0100\n"
            "TZNAME:CET\n"
            "DTSTART:19701025T030000\n"
            "RRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=-1SU;BYMONTH=10\n"
            "END:STANDARD\n"
            "END:VTIMEZONE\n"
            "BEGIN:VEVENT\n"
            "UID:20080407T193125Z-19554-727-1-50@gollum\n"
            "DTSTAMP:20080407T193125Z\n"
            "DTSTART;TZID=/softwarestudio.org/Olson_20011030_5/Europe/Berlin:20080406T090000\n"
            "DTEND;TZID=/softwarestudio.org/Olson_20011030_5/Europe/Berlin:20080406T093000\n"
            "TRANSP:OPAQUE\n"
            "SEQUENCE:XXX\n"
            "SUMMARY:Recurring\n"
            "DESCRIPTION:recurs each Monday\\, 10 times\n"
            "CLASS:PUBLIC\n"
            "RRULE:FREQ=WEEKLY;UNTIL=20080608T070000Z;INTERVAL=1;BYDAY=SU\n"
            "CREATED:20080407T193241Z\n"
            "LAST-MODIFIED:20080407T193241Z\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        config.m_linkedItems[0][1] =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VTIMEZONE\n"
            "TZID:/softwarestudio.org/Olson_20011030_5/Europe/Berlin\n"
            "X-LIC-LOCATION:Europe/Berlin\n"
            "BEGIN:DAYLIGHT\n"
            "TZOFFSETFROM:+0100\n"
            "TZOFFSETTO:+0200\n"
            "TZNAME:CEST\n"
            "DTSTART:19700329T020000\n"
            "RRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=-1SU;BYMONTH=3\n"
            "END:DAYLIGHT\n"
            "BEGIN:STANDARD\n"
            "TZOFFSETFROM:+0200\n"
            "TZOFFSETTO:+0100\n"
            "TZNAME:CET\n"
            "DTSTART:19701025T030000\n"
            "RRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=-1SU;BYMONTH=10\n"
            "END:STANDARD\n"
            "END:VTIMEZONE\n"
            "BEGIN:VEVENT\n"
            "UID:20080407T193125Z-19554-727-1-50@gollum\n"
            "DTSTAMP:20080407T193125Z\n"
            "DTSTART;TZID=/softwarestudio.org/Olson_20011030_5/Europe/Berlin:20080413T090000\n"
            "DTEND;TZID=/softwarestudio.org/Olson_20011030_5/Europe/Berlin:20080413T093000\n"
            "TRANSP:OPAQUE\n"
            "SEQUENCE:XXX\n"
            "SUMMARY:Recurring: Modified\n"
            "CLASS:PUBLIC\n"
            "CREATED:20080407T193241Z\n"
            "LAST-MODIFIED:20080407T193647Z\n"
            "RECURRENCE-ID;TZID=/softwarestudio.org/Olson_20011030_5/Europe/Berlin:20080413T090000\n"
            "DESCRIPTION:second instance modified\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";

	if (server == "funambol") {
	    // converts UNTIL into floating time - broken?!
	    config.m_linkedItems[0][0] =
	        "BEGIN:VCALENDAR\n"
                "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
                "VERSION:2.0\n"
                "BEGIN:VTIMEZONE\n"
                "TZID:/softwarestudio.org/Olson_20011030_5/Europe/Berlin\n"
                "X-LIC-LOCATION:Europe/Berlin\n"
                "BEGIN:DAYLIGHT\n"
                "TZOFFSETFROM:+0100\n"
                "TZOFFSETTO:+0200\n"
                "TZNAME:CEST\n"
                "DTSTART:19700329T020000\n"
                "RRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=-1SU;BYMONTH=3\n"
                "END:DAYLIGHT\n"
                "BEGIN:STANDARD\n"
                "TZOFFSETFROM:+0200\n"
                "TZOFFSETTO:+0100\n"
                "TZNAME:CET\n"
                "DTSTART:19701025T030000\n"
                "RRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=-1SU;BYMONTH=10\n"
                "END:STANDARD\n"
                "END:VTIMEZONE\n"
                "BEGIN:VEVENT\n"
                "UID:20080407T193125Z-19554-727-1-50@gollum\n"
                "DTSTAMP:20080407T193125Z\n"
                "DTSTART;TZID=/softwarestudio.org/Olson_20011030_5/Europe/Berlin:20080406T090000\n"
                "DTEND;TZID=/softwarestudio.org/Olson_20011030_5/Europe/Berlin:20080406T093000\n"
                "TRANSP:OPAQUE\n"
                "SEQUENCE:XXX\n"
                "SUMMARY:Recurring\n"
                "DESCRIPTION:recurs each Monday\\, 10 times\n"
                "CLASS:PUBLIC\n"
                "RRULE:FREQ=WEEKLY;UNTIL=20080608T090000;INTERVAL=1;BYDAY=SU\n"
                "CREATED:20080407T193241Z\n"
                "LAST-MODIFIED:20080407T193241Z\n"
                "END:VEVENT\n"
                "END:VCALENDAR\n";
	} else if (server == "mobical") {
	    // UTC time
	    config.m_linkedItems[0][0] =
	        "BEGIN:VCALENDAR\n"
                "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
                "VERSION:2.0\n"
                "BEGIN:VEVENT\n"
                "UID:20080407T193125Z-19554-727-1-50@gollum\n"
                "DTSTAMP:20080407T193125Z\n"
                "DTSTART:20080406T070000Z\n"
                "DTEND:20080406T073000Z\n"
                "TRANSP:OPAQUE\n"
                "SEQUENCE:XXX\n"
                "SUMMARY:Recurring\n"
                "DESCRIPTION:recurs each Monday\\, 10 times\n"
                "CLASS:PUBLIC\n"
                "RRULE:FREQ=WEEKLY;UNTIL=20080608T070000Z;INTERVAL=1;BYDAY=SU\n"
                "CREATED:20080407T193241Z\n"
                "LAST-MODIFIED:20080407T193241Z\n"
                "END:VEVENT\n"
                "END:VCALENDAR\n";
            config.m_linkedItems[0][1] =
                "BEGIN:VCALENDAR\n"
                "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
                "VERSION:2.0\n"
                "BEGIN:VEVENT\n"
                "UID:20080407T193125Z-19554-727-1-50@gollum\n"
                "DTSTAMP:20080407T193125Z\n"
                "DTSTART:20080413T070000Z\n"
                "DTEND:20080413T073000Z\n"
                "TRANSP:OPAQUE\n"
                "SEQUENCE:XXX\n"
                "SUMMARY:Recurring: Modified\n"
                "CLASS:PUBLIC\n"
                "CREATED:20080407T193241Z\n"
                "LAST-MODIFIED:20080407T193647Z\n"
                "RECURRENCE-ID:20080413T070000Z\n"
                "DESCRIPTION:second instance modified\n"
                "END:VEVENT\n"
                "END:VCALENDAR\n";
	} else if (server == "memotoo") {
	    // local time, except for detached recurrence
	    config.m_linkedItems[0][0] =
	        "BEGIN:VCALENDAR\n"
                "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
                "VERSION:2.0\n"
                "BEGIN:VEVENT\n"
                "UID:20080407T193125Z-19554-727-1-50@gollum\n"
                "DTSTAMP:20080407T193125Z\n"
                "DTSTART:20080406T070000\n"
                "DTEND:20080406T073000\n"
                "TRANSP:OPAQUE\n"
                "SEQUENCE:XXX\n"
                "SUMMARY:Recurring\n"
                "DESCRIPTION:recurs each Monday\\, 10 times\n"
                "CLASS:PUBLIC\n"
                "RRULE:FREQ=WEEKLY;UNTIL=20080608T070000;INTERVAL=1;BYDAY=SU\n"
                "CREATED:20080407T193241Z\n"
                "LAST-MODIFIED:20080407T193241Z\n"
                "END:VEVENT\n"
                "END:VCALENDAR\n";
            config.m_linkedItems[0][1] =
                "BEGIN:VCALENDAR\n"
                "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
                "VERSION:2.0\n"
                "BEGIN:VEVENT\n"
                "UID:20080407T193125Z-19554-727-1-50@gollum\n"
                "DTSTAMP:20080407T193125Z\n"
                "DTSTART:20080413T050000Z\n"
                "DTEND:20080413T053000Z\n"
                "TRANSP:OPAQUE\n"
                "SEQUENCE:XXX\n"
                "SUMMARY:Recurring: Modified\n"
                "CLASS:PUBLIC\n"
                "CREATED:20080407T193241Z\n"
                "LAST-MODIFIED:20080407T193647Z\n"
                "RECURRENCE-ID:20080413T070000\n"
                "DESCRIPTION:second instance modified\n"
                "END:VEVENT\n"
                "END:VCALENDAR\n";
        } else if (server == "exchange") {
            BOOST_FOREACH(std::string &item, config.m_linkedItems[0]) {
                // time zone name changes on server to "Standard Timezone",
                // with some information stripped
                boost::replace_all(item,
                                   "/softwarestudio.org/Olson_20011030_5/Europe/Berlin",
                                   "Standard Timezone");
                // some properties are not stored/supported
                boost::replace_all(item, "TZNAME:CET\n", "");
                boost::replace_all(item, "TZNAME:CEST\n", "");
                boost::replace_all(item, "X-LIC-LOCATION:Europe/Berlin\n", "");
            }

            // also test recurring all-day events with exceptions
            config.m_linkedItems.resize(2);
            config.m_linkedItems[1].resize(2);
            config.m_linkedItems[1][0] =
                "BEGIN:VCALENDAR\n"
                "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
                "VERSION:2.0\n"
                "BEGIN:VEVENT\n"
                "UID:20110829T130000Z-19554-727-1-50@gollum\n"
                "DTSTAMP:20080407T193125Z\n"
                "DTSTART;VALUE=DATE:20080406\n"
                "DTEND;VALUE=DATE:20080407\n"
                "TRANSP:OPAQUE\n"
                "SEQUENCE:XXX\n"
                "SUMMARY:Recurring all day event\n"
                "DESCRIPTION:recurs each Monday\\, 3 times\n"
                "CLASS:PUBLIC\n"
                "RRULE:FREQ=WEEKLY;UNTIL=20080420;INTERVAL=1;BYDAY=SU\n"
                "CREATED:20080407T193241Z\n"
                "LAST-MODIFIED:20080407T193241Z\n"
                "END:VEVENT\n"
                "END:VCALENDAR\n";
            config.m_linkedItems[1][1] =
                "BEGIN:VCALENDAR\n"
                "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
                "VERSION:2.0\n"
                "BEGIN:VEVENT\n"
                "UID:20110829T130000Z-19554-727-1-50@gollum\n"
                "DTSTAMP:20080407T193125Z\n"
                "DTSTART;VALUE=DATE:20080413\n"
                "DTEND;VALUE=DATE:20080414\n"
                "TRANSP:OPAQUE\n"
                "SEQUENCE:XXX\n"
                "SUMMARY:Recurring: Modified second instance\n"
                "CLASS:PUBLIC\n"
                "CREATED:20080407T193241Z\n"
                "LAST-MODIFIED:20080407T193647Z\n"
                "RECURRENCE-ID;VALUE=DATE:20080413\n"
                "DESCRIPTION:second instance modified\n"
                "END:VEVENT\n"
                "END:VCALENDAR\n";
	} else {
            // in particular for Google Calendar: also try with
            // VALARM, because testing showed that the server works
            // differently with and without VALARM data included
            config.m_linkedItems.resize(2);
            config.m_linkedItems[1].resize(2);
            const std::string valarm =
                "BEGIN:VALARM\n"
                "ACTION:DISPLAY\n"
                "DESCRIPTION:This is an event reminder\n"
                "TRIGGER;VALUE=DURATION;RELATED=START:-PT1H\n"
                "X-EVOLUTION-ALARM-UID:foo@bar\n"
                "END:VALARM\nEND:VEVENT";
            config.m_linkedItems[1][0] = config.m_linkedItems[0][0];
            boost::replace_first(config.m_linkedItems[1][0], "END:VEVENT", valarm);
            config.m_linkedItems[1][1] = config.m_linkedItems[0][1];
            boost::replace_first(config.m_linkedItems[1][1], "END:VEVENT", valarm);
        }

        config.m_templateItem = config.m_insertItem;
        config.m_uniqueProperties = "SUMMARY:UID:LOCATION";
        config.m_sizeProperty = "DESCRIPTION";
        config.m_testcases = "testcases/eds_event.ics";
    } else if (!strcmp(type, "eds_event_noutc") ||
               (!strcmp(type, "eds_event") && noutc)) {
        config.m_sourceName = "eds_event";
        config.m_sourceNameServerTemplate = "calendar";
        config.m_uri = "cal2"; // ScheduleWorld
        config.m_type = "text/x-vcalendar";
        config.m_mangleItem = mangleICalendar20;
        config.m_insertItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VTIMEZONE\n"
            "TZID:Asia/Shanghai\n"
            "BEGIN:STANDARD\n"
            "DTSTART:19670101T000000\n"
            "TZOFFSETFROM:+0800\n"
            "TZOFFSETTO:+0800\n"
            "END:STANDARD\n"
            "END:VTIMEZONE\n"
            "BEGIN:VTIMEZONE\n"
            "TZID:/freeassociation.sourceforge.net/Tzfile/Asia/Shanghai\n"
            "X-LIC-LOCATION:Asia/Shanghai\n"
            "BEGIN:STANDARD\n"
            "TZNAME:CST\n"
            "DTSTART:19700914T230000\n"
            "TZOFFSETFROM:+0800\n"
            "TZOFFSETTO:+0800\n"
            "END:STANDARD\n"
            "END:VTIMEZONE\n"
            "BEGIN:VEVENT\n"
            "SUMMARY:phone meeting\n"
            "DTEND;TZID=/freeassociation.sourceforge.net/Tzfile/Asia/Shanghai:20060406T163000\n"
            "DTSTART;TZID=/freeassociation.sourceforge.net/Tzfile/Asia/Shanghai:20060406T160000\n"
            "UID:1234567890!@#$%^&*()<>@dummy\n"
            "DTSTAMP:20060406T211449Z\n"
            "LAST-MODIFIED:20060409T213201Z\n"
            "CREATED:20060409T213201Z\n"
            "LOCATION:my office\n"
            "DESCRIPTION:let's talk<<REVISION>>\n"
            "CLASS:PUBLIC\n"
            "TRANSP:OPAQUE\n"
            "SEQUENCE:XXX\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        config.m_updateItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VTIMEZONE\n"
            "TZID:Asia/Shanghai\n"
            "BEGIN:STANDARD\n"
            "DTSTART:19670101T000000\n"
            "TZOFFSETFROM:+0800\n"
            "TZOFFSETTO:+0800\n"
            "END:STANDARD\n"
            "END:VTIMEZONE\n"
            "BEGIN:VTIMEZONE\n"
            "TZID:/freeassociation.sourceforge.net/Tzfile/Asia/Shanghai\n"
            "X-LIC-LOCATION:Asia/Shanghai\n"
            "BEGIN:STANDARD\n"
            "TZNAME:CST\n"
            "DTSTART:19700914T230000\n"
            "TZOFFSETFROM:+0800\n"
            "TZOFFSETTO:+0800\n"
            "END:STANDARD\n"
            "END:VTIMEZONE\n"
            "BEGIN:VEVENT\n"
            "SUMMARY:meeting on site\n"
            "DTEND;TZID=/freeassociation.sourceforge.net/Tzfile/Asia/Shanghai:20060406T163000\n"
            "DTSTART;TZID=/freeassociation.sourceforge.net/Tzfile/Asia/Shanghai:20060406T160000\n"
            "UID:1234567890!@#$%^&*()<>@dummy\n"
            "DTSTAMP:20060406T211449Z\n"
            "LAST-MODIFIED:20060409T213201Z\n"
            "CREATED:20060409T213201Z\n"
            "LOCATION:big meeting room\n"
            "DESCRIPTION:nice to see you\n"
            "CLASS:PUBLIC\n"
            "TRANSP:OPAQUE\n"
            "SEQUENCE:XXX\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        /* change location and description of insertItem in testMerge(), add alarm */
        config.m_mergeItem1 = "";
        config.m_mergeItem2 = "";
        config.m_templateItem = config.m_insertItem;
        config.m_uniqueProperties = "SUMMARY:UID:LOCATION";
        config.m_sizeProperty = "DESCRIPTION";
        config.m_testcases = "testcases/eds_event.ics";
    } else if(!strcmp(type, "eds_task")) {
        config.m_sourceName = "eds_task";
        config.m_sourceNameServerTemplate = "todo";
        config.m_uri = "task2"; // ScheduleWorld
        config.m_type = "text/x-vcalendar";
        config.m_mangleItem = mangleICalendar20;
        config.m_insertItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VTODO\n"
            "UID:20060417T173712Z-4360-727-1-2730@gollum\n"
            "DTSTAMP:20060417T173712Z\n"
            "SUMMARY:do me\n"
            "DESCRIPTION:to be done<<REVISION>>\n"
            "PRIORITY:0\n"
            "STATUS:NEEDS-ACTION\n"
            "CREATED:20060417T173712Z\n"
            "LAST-MODIFIED:20060417T173712Z\n"
            "END:VTODO\n"
            "END:VCALENDAR\n";
        config.m_updateItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VTODO\n"
            "UID:20060417T173712Z-4360-727-1-2730@gollum\n"
            "DTSTAMP:20060417T173712Z\n"
            "SUMMARY:do me ASAP\n"
            "DESCRIPTION:to be done\n"
            "PRIORITY:1\n"
            "STATUS:NEEDS-ACTION\n"
            "CREATED:20060417T173712Z\n"
            "LAST-MODIFIED:20060417T173712Z\n"
            "END:VTODO\n"
            "END:VCALENDAR\n";
        /* change summary in insertItem in testMerge() */
        config.m_mergeItem1 =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VTODO\n"
            "UID:20060417T173712Z-4360-727-1-2730@gollum\n"
            "DTSTAMP:20060417T173712Z\n"
            "SUMMARY:do me please\\, please\n"
            "DESCRIPTION:to be done\n"
            "PRIORITY:0\n"
            "STATUS:NEEDS-ACTION\n"
            "CREATED:20060417T173712Z\n"
            "LAST-MODIFIED:20060417T173712Z\n"
            "END:VTODO\n"
            "END:VCALENDAR\n";
        config.m_mergeItem2 =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VTODO\n"
            "UID:20060417T173712Z-4360-727-1-2730@gollum\n"
            "DTSTAMP:20060417T173712Z\n"
            "SUMMARY:do me\n"
            "DESCRIPTION:to be done\n"
            "PRIORITY:7\n"
            "STATUS:NEEDS-ACTION\n"
            "CREATED:20060417T173712Z\n"
            "LAST-MODIFIED:20060417T173712Z\n"
            "END:VTODO\n"
            "END:VCALENDAR\n";
        config.m_templateItem = config.m_insertItem;
        config.m_uniqueProperties = "SUMMARY:UID";
        config.m_sizeProperty = "DESCRIPTION";
        config.m_testcases = "testcases/eds_task.ics";
    } else if(!strcmp(type, "eds_memo")) {
        // The "eds_memo" test uses iCalendar 2.0 VJOURNAL
        // as format because synccompare doesn't handle
        // plain text. A backend which wants to use this
        // test data must support importing/exporting
        // the test data in that format, see EvolutionMemoSource
        // for an example.
        config.m_uri = "note"; // ScheduleWorld
        config.m_sourceName = "eds_memo";
        config.m_sourceNameServerTemplate = "memo";
        config.m_type = "memo";
        config.m_itemType = "text/calendar";
        config.m_mangleItem = mangleICalendar20;
        config.m_insertItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VJOURNAL\n"
            "SUMMARY:Summary\n"
            "DESCRIPTION:Summary\\nBody text\n"
            "END:VJOURNAL\n"
            "END:VCALENDAR\n";
        config.m_updateItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VJOURNAL\n"
            "SUMMARY:Summary Modified\n"
            "DESCRIPTION:Summary Modified\\nBody text\n"
            "END:VJOURNAL\n"
            "END:VCALENDAR\n";
        /* change summary, as in updateItem, and the body in the other merge item */
        config.m_mergeItem1 = config.m_updateItem;
        config.m_mergeItem2 =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VJOURNAL\n"
            "SUMMARY:Summary\n"
            "DESCRIPTION:Summary\\nBody modified\n"
            "END:VJOURNAL\n"
            "END:VCALENDAR\n";                
        config.m_templateItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "BEGIN:VJOURNAL\n"
            "SUMMARY:Summary\n"
            "DESCRIPTION:Summary\\nBody text <<REVISION>>\n"
            "END:VJOURNAL\n"
            "END:VCALENDAR\n";
        config.m_uniqueProperties = "SUMMARY:DESCRIPTION";
        config.m_sizeProperty = "DESCRIPTION";
        config.m_testcases = "testcases/eds_memo.ics";
    }else if (!strcmp (type, "calendar+todo")) {
        config.m_uri="";
        config.m_sourceNameServerTemplate = "calendar+todo";
    }
}

void CheckSyncReport::check(SyncMLStatus status, SyncReport &report) const
{
    if (m_report) {
        *m_report = report;
    }

    stringstream str;

    str << report;
    str << "----------|--------CLIENT---------|--------SERVER---------|\n";
    str << "          |  NEW  |  MOD  |  DEL  |  NEW  |  MOD  |  DEL  |\n";
    str << "----------|-----------------------------------------------|\n";
    str << StringPrintf("Expected  |  %3d  |  %3d  |  %3d  |  %3d  |  %3d  |  %3d  |\n",
                        clientAdded, clientUpdated, clientDeleted,
                        serverAdded, serverUpdated, serverDeleted);
    str << "Expected sync mode: " << PrettyPrintSyncMode(syncMode) << "\n";
    SE_LOG_INFO(NULL, NULL, "sync report:\n%s\n", str.str().c_str());

    if (mustSucceed) {
        // both STATUS_OK and STATUS_HTTP_OK map to the same
        // string, so check the formatted status first, then
        // the numerical one
        CPPUNIT_ASSERT_EQUAL(string("no error (remote, status 0)"), Status2String(status));
        CPPUNIT_ASSERT_EQUAL(STATUS_OK, status);
    }

    // this code is intentionally duplicated to produce nicer CPPUNIT asserts
    BOOST_FOREACH(SyncReport::value_type &entry, report) {
        const std::string &name = entry.first;
        const SyncSourceReport &source = entry.second;

        SE_LOG_DEBUG(NULL, NULL, "Checking sync source %s...", name.c_str());
        if (mustSucceed) {
            CLIENT_TEST_EQUAL(name, STATUS_OK, source.getStatus());
        }
        CLIENT_TEST_EQUAL(name, 0, source.getItemStat(SyncSourceReport::ITEM_LOCAL,
                                                      SyncSourceReport::ITEM_ANY,
                                                      SyncSourceReport::ITEM_REJECT));
        CLIENT_TEST_EQUAL(name, 0, source.getItemStat(SyncSourceReport::ITEM_REMOTE,
                                                      SyncSourceReport::ITEM_ANY,
                                                      SyncSourceReport::ITEM_REJECT));

        const char* checkSyncModeStr = getenv("CLIENT_TEST_NOCHECK_SYNCMODE");
        bool checkSyncMode = true;
        bool checkSyncStats = getenv ("CLIENT_TEST_NOCHECK_SYNCSTATS") ? false : true;
        if (checkSyncModeStr && 
                (!strcmp(checkSyncModeStr, "1") || !strcasecmp(checkSyncModeStr, "t"))) {
            checkSyncMode = false;
        }

        if (syncMode != SYNC_NONE && checkSyncMode) {
            CLIENT_TEST_EQUAL(name, syncMode, source.getFinalSyncMode());
        }

        if (clientAdded != -1 && checkSyncStats) {
            CLIENT_TEST_EQUAL(name, clientAdded,
                              source.getItemStat(SyncSourceReport::ITEM_LOCAL,
                                                 SyncSourceReport::ITEM_ADDED,
                                                 SyncSourceReport::ITEM_TOTAL));
        }
        if (clientUpdated != -1 && checkSyncStats) {
            CLIENT_TEST_EQUAL(name, clientUpdated,
                              source.getItemStat(SyncSourceReport::ITEM_LOCAL,
                                                 SyncSourceReport::ITEM_UPDATED,
                                                 SyncSourceReport::ITEM_TOTAL));
        }
        if (clientDeleted != -1 && checkSyncStats) {
            CLIENT_TEST_EQUAL(name, clientDeleted,
                              source.getItemStat(SyncSourceReport::ITEM_LOCAL,
                                                 SyncSourceReport::ITEM_REMOVED,
                                                 SyncSourceReport::ITEM_TOTAL));
        }

        if (serverAdded != -1 && checkSyncStats) {
            CLIENT_TEST_EQUAL(name, serverAdded,
                              source.getItemStat(SyncSourceReport::ITEM_REMOTE,
                                                 SyncSourceReport::ITEM_ADDED,
                                                 SyncSourceReport::ITEM_TOTAL));
        }
        if (serverUpdated != -1 && checkSyncStats) {
            CLIENT_TEST_EQUAL(name, serverUpdated,
                              source.getItemStat(SyncSourceReport::ITEM_REMOTE,
                                                 SyncSourceReport::ITEM_UPDATED,
                                                 SyncSourceReport::ITEM_TOTAL));
        }
        if (serverDeleted != -1 && checkSyncStats) {
            CLIENT_TEST_EQUAL(name, serverDeleted,
                              source.getItemStat(SyncSourceReport::ITEM_REMOTE,
                                                 SyncSourceReport::ITEM_REMOVED,
                                                 SyncSourceReport::ITEM_TOTAL));
        }
    }
    SE_LOG_DEBUG(NULL, NULL, "Done with checking sync report.");
}

/** @} */
/** @endcond */
#endif // ENABLE_INTEGRATION_TESTS

SE_END_CXX
