/*
 * Copyright (C) 2008-2009 Patrick Ohly <patrick.ohly@gmx.de>
 * Copyright (C) 2010 Intel Corporation
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

#include "KCalExtendedSource.h"
#include "test.h"

#include <syncevo/declarations.h>
SE_BEGIN_CXX

static SyncSource *createSource(const SyncSourceParams &params)
{
    SourceType sourceType = SyncSource::getSourceType(params.m_nodes);
    bool isMe = sourceType.m_backend == "mkcal-events";
    bool maybeMe = sourceType.m_backend == "calendar";

    if (isMe || maybeMe) {
        if (sourceType.m_format == "" ||
            sourceType.m_format == "text/x-vcalendar" ||
            sourceType.m_format == "text/x-calendar" ||
            sourceType.m_format == "text/calendar") {
            return
#ifdef ENABLE_KCALEXTENDED
                true ? new KCalExtendedSource(params, KCalExtendedSource::Event) :
#endif
                isMe ? RegisterSyncSource::InactiveSource(params) : NULL;
        }
    }

    isMe = sourceType.m_backend == "mkcal-todos";
    maybeMe = sourceType.m_backend == "todo";

    if (isMe || maybeMe) {
        if (sourceType.m_format == "" ||
            sourceType.m_format == "text/x-vcalendar" ||
            sourceType.m_format == "text/x-calendar" ||
            sourceType.m_format == "text/calendar") {
            return
#ifdef ENABLE_KCALEXTENDED
                true ? new KCalExtendedSource(params, KCalExtendedSource::Todo) :
#endif
                isMe ? RegisterSyncSource::InactiveSource(params) : NULL;
        }
    }

    isMe = sourceType.m_backend == "mkcal-notes";
    maybeMe = sourceType.m_backend == "memo";

    if (isMe || maybeMe) {
        if (sourceType.m_format == "" ||
            sourceType.m_format == "text/x-vcalendar" ||
            sourceType.m_format == "text/x-calendar" ||
            sourceType.m_format == "text/calendar") {
            return
#ifdef ENABLE_KCALEXTENDED
                true ? new KCalExtendedSource(params, KCalExtendedSource::Journal) :
#endif
                isMe ? RegisterSyncSource::InactiveSource(params) : NULL;
        }
    }

    return NULL;
}

static RegisterSyncSource registerMe("KCalExtended",
#ifdef ENABLE_KCALEXTENDED
                                     true,
#else
                                     false,
#endif
                                     createSource,
                                     "mkcal-events = mkcal = KCalExtended = calendar\n"
                                     "   'database' normally is the name of a calendar\n"
                                     "   inside the default calendar storage. If it starts\n" 
                                     "   with the 'SyncEvolution_Test_' prefix, it will be\n"
                                     "   created as needed, otherwise it must exist.\n"
#ifdef ENABLE_MAEMO
                                     "   If it starts with the 'uid:' prefix, the specified\n"
                                     "   calendar in the default SQLite storage file will\n"
                                     "   be used. It must exist.\n"
#endif
                                     "   If it starts with the 'file://' prefix, the default\n"
                                     "   calendar in the specified SQLite storage file will\n"
                                     "   created (if needed) and used.\n"
                                     "mkcal-todos = todo\n"
                                     "   Same as above.\n"
                                     "mkcal-notes = memo\n"
#ifdef ENABLE_MAEMO
                                     "   Same as above. Keep in mind that, by default, notes\n"
                                     "   are stored in their own database, separate from\n"
                                     "   events and todos. The name of this database is\n"
                                     "   hardcoded into the device's builtin Notes app.\n"
                                     "   Don't override the default unless you know what\n"
                                     "   you are doing.\n",
#else
                                     "   Same as above.\n",
#endif
                                     Values() +
                                     (Aliases("mkcal-events") + "mkcal" + "KCalExtended" + "MeeGo Calendar") +
                                     (Aliases("mkcal-todos") + "MeeGo Tasks") +
                                     (Aliases("mkcal-notes") + "MeeGo Notes"));

#ifdef ENABLE_KCALEXTENDED
#ifdef ENABLE_UNIT_TESTS

class KCalExtendedSourceUnitTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(KCalExtendedSourceUnitTest);
    CPPUNIT_TEST(testInstantiate);
    CPPUNIT_TEST_SUITE_END();

protected:
    void testInstantiate() {
        boost::shared_ptr<SyncSource> source;
        source.reset(SyncSource::createTestingSource("KCalExtended", "KCalExtended:text/calendar:2.0", true));
    }
};

SYNCEVOLUTION_TEST_SUITE_REGISTRATION(KCalExtendedSourceUnitTest);

#endif // ENABLE_UNIT_TESTS

namespace {
#if 0
}
#endif

static class ICal20Test : public RegisterSyncSourceTest {
public:
    ICal20Test() : RegisterSyncSourceTest("kcal_event", "eds_event") {}

    virtual void updateConfig(ClientTestConfig &config) const
    {
        config.m_type = "KCalExtended:text/calendar";
        // after fixing BMC #6061, mKCal is able to delete individual
        // VEVENTs, without enforcing the "each child must have parent" rule
        config.m_linkedItemsRelaxedSemantic = true;
    }
} iCal20Test;

}

#endif // ENABLE_KCALEXTENDED

SE_END_CXX
