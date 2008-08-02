/*
 * Copyright (C) 2005-2006 Patrick Ohly
 * Copyright (C) 2007 Funambol
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <memory>
using namespace std;

#include "config.h"

#ifdef ENABLE_ECAL

// include first, it sets HANDLE_LIBICAL_MEMORY for us
#include "libical/icalstrdup.h"

#include "EvolutionSyncClient.h"
#include "EvolutionCalendarSource.h"
#include "EvolutionMemoSource.h"
#include "EvolutionSmartPtr.h"
#include "e-cal-check-timezones.h"

#include <common/base/Log.h>

#include <boost/foreach.hpp>

static const string
EVOLUTION_CALENDAR_PRODID("PRODID:-//ACME//NONSGML SyncEvolution//EN"),
EVOLUTION_CALENDAR_VERSION("VERSION:2.0");

class unrefECalObjectList {
 public:
    /** free list of ECalChange instances */
    static void unref(GList *pointer) {
        if (pointer) {
            e_cal_free_object_list(pointer);
        }
    }
};


EvolutionCalendarSource::EvolutionCalendarSource(ECalSourceType type,
                                                 const EvolutionSyncSourceParams &params) :
    TrackingSyncSource(params),
    m_type(type)
{
}

EvolutionCalendarSource::EvolutionCalendarSource( const EvolutionCalendarSource &other ) :
    TrackingSyncSource(other),
    m_type(other.m_type)
{
    switch (m_type) {
     case E_CAL_SOURCE_TYPE_EVENT:
        m_typeName = "calendar";
        m_newSystem = e_cal_new_system_calendar;
        break;
     case E_CAL_SOURCE_TYPE_TODO:
        m_typeName = "task list";
        m_newSystem = e_cal_new_system_tasks;
        break;
     case E_CAL_SOURCE_TYPE_JOURNAL:
        m_typeName = "memo list";
        // This is not available in older Evolution versions.
        // A configure check could detect that, but as this isn't
        // important the functionality is simply disabled.
        m_newSystem = NULL /* e_cal_new_system_memos */;
        break;
     default:
        EvolutionSyncClient::throwError("internal error, invalid calendar type");
        break;
    }
}

EvolutionSyncSource::Databases EvolutionCalendarSource::getDatabases()
{
    ESourceList *sources = NULL;
    GError *gerror = NULL;
    Databases result;

    if (!e_cal_get_sources(&sources, m_type, &gerror)) {
        // ignore unspecific errors (like on Maemo with no support for memos)
        // and simply return an empty list
        if (!gerror) {
            return result;
        }
        throwError("unable to access backend databases", gerror);
    }

    bool first = true;
    for (GSList *g = e_source_list_peek_groups (sources); g; g = g->next) {
        ESourceGroup *group = E_SOURCE_GROUP (g->data);
        for (GSList *s = e_source_group_peek_sources (group); s; s = s->next) {
            ESource *source = E_SOURCE (s->data);
            eptr<char> uri(e_source_get_uri(source));
            result.push_back(Database(e_source_peek_name(source),
                                      uri ? uri.get() : "",
                                      first));
            first = false;
        }
    }
    return result;
}

char *EvolutionCalendarSource::authenticate(const char *prompt,
                                            const char *key)
{
    const char *passwd = getPassword();

    LOG.debug("%s: authentication requested, prompt \"%s\", key \"%s\" => %s",
              getName(), prompt, key,
              passwd && passwd[0] ? "returning configured password" : "no password configured");
    return passwd && passwd[0] ? strdup(passwd) : NULL;
}

void EvolutionCalendarSource::open()
{
    ESourceList *sources;
    GError *gerror = NULL;

    if (!e_cal_get_sources(&sources, m_type, &gerror)) {
        throwError("unable to access backend databases", gerror);
    }

    string id = getDatabaseID();    
    ESource *source = findSource(sources, id);
    bool onlyIfExists = true;
    if (!source) {
        // might have been special "<<system>>" or "<<default>>", try that and
        // creating address book from file:// URI before giving up
        if (id == "<<system>>" && m_newSystem) {
            m_calendar.set(m_newSystem(), (string("system ") + m_typeName).c_str());
        } else if (!id.compare(0, 7, "file://")) {
            m_calendar.set(e_cal_new_from_uri(id.c_str(), m_type), (string("creating ") + m_typeName).c_str());
        } else {
            throwError(string("not found: '") + id + "'");
        }
        onlyIfExists = false;
    } else {
        m_calendar.set(e_cal_new(source, m_type), m_typeName.c_str());
    }

    e_cal_set_auth_func(m_calendar, eCalAuthFunc, this);
    
    if (!e_cal_open(m_calendar, onlyIfExists, &gerror)) {
        // opening newly created address books often failed, perhaps that also applies to calendars - try again
        g_clear_error(&gerror);
        sleep(5);
        if (!e_cal_open(m_calendar, onlyIfExists, &gerror)) {
            throwError(string("opening ") + m_typeName, gerror );
        }
    }

    g_signal_connect_after(m_calendar,
                           "backend-died",
                           G_CALLBACK(EvolutionSyncClient::fatalError),
                           (void *)"Evolution Data Server has died unexpectedly, database no longer available.");
}

void EvolutionCalendarSource::listAllItems(RevisionMap_t &revisions)
{
    GError *gerror = NULL;
    GList *nextItem;

    m_allLUIDs.clear();
    if (!e_cal_get_object_list_as_comp(m_calendar,
                                       "#t",
                                       &nextItem,
                                       &gerror)) {
        throwError( "reading all items", gerror );
    }
    eptr<GList> listptr(nextItem);
    while (nextItem) {
        ECalComponent *ecomp = E_CAL_COMPONENT(nextItem->data);
        ItemID id = getItemID(ecomp);
        string luid = id.getLUID();
        string modTime = getItemModTime(ecomp);

        m_allLUIDs.insert(luid);
        revisions[luid] = modTime;
        nextItem = nextItem->next;
    }
}

void EvolutionCalendarSource::close()
{
    m_calendar = NULL;
}

void EvolutionCalendarSource::exportData(ostream &out)
{
    GList *nextItem;
    GError *gerror = NULL;

    if (!e_cal_get_object_list_as_comp(m_calendar,
                                       "(contains? \"any\" \"\")",
                                       &nextItem,
                                       &gerror)) {
        throwError( "reading all items", gerror );
    }
    eptr<GList> listptr(nextItem);
    while (nextItem) {
        ItemID id = getItemID(E_CAL_COMPONENT(nextItem->data));
        out << retrieveItemAsString(id);
        out << "\r\n";
        nextItem = nextItem->next;
    }
}

SyncItem *EvolutionCalendarSource::createItem(const string &luid)
{
    logItem( luid, "extracting from EV", true );

    ItemID id = ItemID::parseLUID(luid);
    string icalstr = retrieveItemAsString(id);

    auto_ptr<SyncItem> item(new SyncItem(luid.c_str()));
    item->setData(icalstr.c_str(), icalstr.size());
    item->setDataType("text/calendar");
    item->setModificationTime(0);

    return item.release();
}

void EvolutionCalendarSource::setItemStatusThrow(const char *key, int status)
{
    switch (status) {
    case STC_CONFLICT_RESOLVED_WITH_SERVER_DATA:
         LOG.error("%s: item %.80s: conflict, will be replaced by server\n",
                   getName(), key);
        break;
    }
    TrackingSyncSource::setItemStatusThrow(key, status);
}

EvolutionCalendarSource::InsertItemResult EvolutionCalendarSource::insertItem(const string &luid, const SyncItem &item)
{
    bool update = !luid.empty();
    bool merged = false;
    bool detached = false;
    string newluid = luid;
    string data = (const char *)item.getData();
    string modTime;

    /*
     * Evolution/libical can only deal with \, as separator.
     * Replace plain , in incoming event CATEGORIES with \, -
     * based on simple text search/replace and thus will not work
     * in all cases...
     *
     * Inverse operation in extractItemAsString().
     */
    size_t propstart = data.find("\nCATEGORIES");
    bool modified = false;
    while (propstart != data.npos) {
        size_t eol = data.find('\n', propstart + 1);
        size_t comma = data.find(',', propstart);

        while (eol != data.npos &&
               comma != data.npos &&
               comma < eol) {
            if (data[comma-1] != '\\') {
                data.insert(comma, "\\");
                comma++;
                modified = true;
            }
            comma = data.find(',', comma + 1);
        }
        propstart = data.find("\nCATEGORIES", propstart + 1);
    }
    if (modified) {
        LOG.debug("after replacing , with \\, in CATEGORIES:\n%s", data.c_str());
    }

    eptr<icalcomponent> icomp(icalcomponent_new_from_string((char *)data.c_str()));

    if( !icomp ) {
        throwError( string( "parsing ical" ) + data,
                    NULL );
    }

    GError *gerror = NULL;

    // fix up TZIDs
    if (!e_cal_check_timezones(icomp,
                               NULL,
                               e_cal_tzlookup_ecal,
                               (const void *)m_calendar.get(),
                               &gerror)) {
        throwError(string("fixing timezones") + data,
                   gerror);
    }

    // insert before adding/updating the event so that the new VTIMEZONE is
    // immediately available should anyone want it
    for (icalcomponent *tcomp = icalcomponent_get_first_component(icomp, ICAL_VTIMEZONE_COMPONENT);
         tcomp;
         tcomp = icalcomponent_get_next_component(icomp, ICAL_VTIMEZONE_COMPONENT)) {
        eptr<icaltimezone> zone(icaltimezone_new(), "icaltimezone");
        icaltimezone_set_component(zone, tcomp);

        GError *gerror = NULL;
        gboolean success = e_cal_add_timezone(m_calendar, zone, &gerror);
        if (!success) {
            throwError(string("error adding VTIMEZONE ") + icaltimezone_get_tzid(zone),
                       gerror);
        }
    }

    // the component to update/add must be the
    // ICAL_VEVENT/VTODO_COMPONENT of the item,
    // e_cal_create/modify_object() fail otherwise
    icalcomponent *subcomp = icalcomponent_get_first_component(icomp,
                                                               getCompType());
    if (!subcomp) {
        throwError("extracting event");
    }
    
    if (!update) {
        ItemID id = getItemID(subcomp);
        const char *uid = NULL;

        // Trying to add a normal event which already exists leads to a
        // gerror->domain == E_CALENDAR_ERROR
        // gerror->code == E_CALENDAR_STATUS_OBJECT_ID_ALREADY_EXISTS
        // error. Depending on the Evolution version, the subcomp
        // UID gets removed (>= 2.12) or remains unchanged.
        //
        // Existing detached recurrences are silently updated when
        // trying to add them. This breaks our return code and change
        // tracking.
        //
        // Escape this madness by checking the existence ourselve first
        // based on our list of existing LUIDs. Note that this list is
        // not updated during a sync. This is correct as long as no LUID
        // gets used twice during a sync (examples: add + add, delete + add),
        // which should never happen.
        newluid = id.getLUID();
        if (m_allLUIDs.find(newluid) != m_allLUIDs.end()) {
            logItem(item, "exists already, updating instead");
            merged = true;
        } else {
            // if this is a detached recurrence, then we
            // must use e_cal_modify_object() below if
            // the parent already exists
            if (!id.m_rid.empty() &&
                m_allLUIDs.find(ItemID::getLUID(id.m_uid, "")) != m_allLUIDs.end()) {
                detached = true;
            } else {
                // Creating the parent while children are already in
                // the calendar confuses EDS (at least 2.12): the
                // parent is stored in the .ics with the old UID, but
                // the uid returned to the caller is a different
                // one. Retrieving the item then fails. Avoid this
                // problem by removing the children from the calendar,
                // adding the parent, then updating it with the
                // saved children.
                ICalComps_t children;
                if (id.m_rid.empty()) {
                    children = removeEvents(id.m_uid, true);
                }

                // creating new objects works for normal events and detached occurrences alike
                if(e_cal_create_object(m_calendar, subcomp, (char **)&uid, &gerror)) {
                    // Evolution workaround: don't rely on uid being set if we already had
                    // one. In Evolution 2.12.1 it was set to garbage. The recurrence ID
                    // shouldn't have changed either.
                    ItemID newid(!id.m_uid.empty() ? id.m_uid : uid, id.m_rid);
                    newluid = newid.getLUID();
                    modTime = getItemModTime(newid);
                    m_allLUIDs.insert(newluid);
                } else {
                    throwError("storing new item", gerror);
                }

                // Recreate any children removed earlier: when we get here,
                // the parent exists and we must update it.
                BOOST_FOREACH(boost::shared_ptr< eptr<icalcomponent> > &icalcomp, children) {
                    if (!e_cal_modify_object(m_calendar, *icalcomp,
                                             CALOBJ_MOD_THIS,
                                             &gerror)) {
                        throwError(string("recreating item ") + item.getKey(), gerror);
                    }
                }
            }
        }
    }

    if (update || merged || detached) {
        ItemID id = ItemID::parseLUID(newluid);

        // ensure that the component has the right UID
        if (update && !id.m_uid.empty()) {
            icalcomponent_set_uid(subcomp, id.m_uid.c_str());
        }

        if (!e_cal_modify_object(m_calendar, subcomp,
                                 CALOBJ_MOD_THIS,
                                 &gerror)) {
            throwError(string("updating item ") + item.getKey(), gerror);
        }
        ItemID newid = getItemID(subcomp);
        newluid = newid.getLUID();
        modTime = getItemModTime(newid);
    }

    return InsertItemResult(newluid, modTime, merged);
}

EvolutionCalendarSource::ICalComps_t EvolutionCalendarSource::removeEvents(const string &uid, bool returnOnlyChildren)
{
    ICalComps_t events;

    BOOST_FOREACH(const string &luid, m_allLUIDs) {
        ItemID id = ItemID::parseLUID(luid);

        if (id.m_uid == uid) {
            icalcomponent *icomp = retrieveItem(id);
            if (icomp) {
                if (id.m_rid.empty() && returnOnlyChildren) {
                    icalcomponent_free(icomp);
                } else {
                    events.push_back(ICalComps_t::value_type(new eptr<icalcomponent>(icomp)));
                }
            }
        }
    }

    // removes all events with that UID, including children
    GError *gerror = NULL;
    if(!e_cal_remove_object(m_calendar,
                            uid.c_str(),
                            &gerror)) {
        if (gerror->domain == E_CALENDAR_ERROR &&
            gerror->code == E_CALENDAR_STATUS_OBJECT_NOT_FOUND) {
            LOG.debug("%s: %s: request to delete non-existant item ignored",
                      getName(), uid.c_str());
            g_clear_error(&gerror);
        } else {
            throwError(string("deleting item " ) + uid, gerror);
        }
    }

    return events;
}

void EvolutionCalendarSource::deleteItem(const string &luid)
{
    GError *gerror = NULL;
    ItemID id = ItemID::parseLUID(luid);

    if (id.m_rid.empty()) {
        /*
         * Removing the parent item also removes all children. Evolution
         * does that automatically. Calling e_cal_remove_object_with_mod()
         * without valid rid confuses Evolution, don't do it. As a workaround
         * remove all items with the given uid and if we only wanted to
         * delete the parent, then recreate the children.
         */
        ICalComps_t children = removeEvents(id.m_uid, true);

        // recreate children
        BOOST_FOREACH(boost::shared_ptr< eptr<icalcomponent> > &icalcomp, children) {
            char *uid;

            if (!e_cal_create_object(m_calendar, *icalcomp, &uid, &gerror)) {
                throwError(string("recreating item ") + luid, gerror);
            }
        }
    } else if(!e_cal_remove_object_with_mod(m_calendar,
                                            id.m_uid.c_str(),
                                            id.m_rid.c_str(),
                                            CALOBJ_MOD_THIS,
                                            &gerror)) {
        if (gerror->domain == E_CALENDAR_ERROR &&
            gerror->code == E_CALENDAR_STATUS_OBJECT_NOT_FOUND) {
            LOG.debug("%s: %s: request to delete non-existant item ignored",
                      getName(), luid.c_str());
            g_clear_error(&gerror);
        } else {
            throwError(string("deleting item " ) + luid, gerror);
        }
    }
    m_allLUIDs.erase(luid);
}

void EvolutionCalendarSource::flush()
{
    // Flushing is not necessary, all changes are directly stored.
    // However, our change tracking depends on the resolution with which
    // Evolution stores modification times. Sleeping for a second here
    // ensures that any future operations on the same date generate
    // different modification time stamps.
    time_t start = time(NULL);
    do {
        sleep(1);
    } while (time(NULL) - start <= 0);
}

void EvolutionCalendarSource::logItem(const string &luid, const string &info, bool debug)
{
    if (LOG.getLevel() >= (debug ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO)) {
        (LOG.*(debug ? &Log::debug : &Log::info))("%s: %s: %s", getName(), luid.c_str(), info.c_str());
    }
}

/**
 * quick and dirty parser for simple, single-line properties
 * @param keyword   must include leading \n and trailing :
 */
static string extractProp(const char *data, const char *keyword)
{
    string prop;

    const char *keyptr = strstr(data, keyword);
    if (keyptr) {
        const char *end = strpbrk(keyptr + 1, "\n\r");
        if (end) {
            prop.assign(keyptr + strlen(keyword), end - keyptr - strlen(keyword));
        } else {
            prop.assign(keyptr + strlen(keyword));
        }
    }
    return prop;
}

void EvolutionCalendarSource::logItem(const SyncItem &item, const string &info, bool debug)
{
    if (LOG.getLevel() >= (debug ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO)) {
        const char *keyptr = item.getKey();
        string key;
        if (!keyptr || !keyptr[0]) {
            // get UID from data via simple string search; doesn't have to be perfect
            const char *data = (const char *)item.getData();
            string uid = extractProp(data, "\nUID:");
            string rid = extractProp(data, "\nRECURRENCE-ID:");
            if (uid.empty()) {
                key = "<<no UID>>";
            } else {
                key = ItemID::getLUID(uid, rid);
            }
        } else {
            key = keyptr;
        }
        (LOG.*(debug ? &Log::debug : &Log::info))("%s: %s: %s", getName(), key.c_str(), info.c_str());
    }
}

icalcomponent *EvolutionCalendarSource::retrieveItem(const ItemID &id)
{
    GError *gerror = NULL;
    icalcomponent *comp;

    if (!e_cal_get_object(m_calendar,
                          id.m_uid.c_str(),
                          !id.m_rid.empty() ? id.m_rid.c_str() : NULL,
                          &comp,
                          &gerror)) {
        throwError(string("retrieving item: ") + id.getLUID(), gerror);
    }
    if (!comp) {
        throwError(string("retrieving item: ") + id.getLUID());
    }

    return comp;
}

string EvolutionCalendarSource::retrieveItemAsString(const ItemID &id)
{
    eptr<icalcomponent> comp(retrieveItem(id));
    eptr<char> icalstr;

    icalstr = e_cal_get_component_as_string(m_calendar, comp);
    if (!icalstr) {
        throwError(string("could not encode item as iCal: ") + id.getLUID());
    }

    /*
     * Evolution/libical can only deal with \, as separator.
     * Replace plain \, in outgoing event CATEGORIES with , -
     * based on simple text search/replace and thus will not work
     * in all cases...
     *
     * Inverse operation in insertItem().
     */
    string data = string(icalstr);
    size_t propstart = data.find("\nCATEGORIES");
    bool modified = false;
    while (propstart != data.npos) {
        size_t eol = data.find('\n', propstart + 1);
        size_t comma = data.find(',', propstart);

        while (eol != data.npos &&
               comma != data.npos &&
               comma < eol) {
            if (data[comma-1] == '\\') {
                data.erase(comma - 1, 1);
                comma--;
                modified = true;
            }
            comma = data.find(',', comma + 1);
        }
        propstart = data.find("\nCATEGORIES", propstart + 1);
    }
    if (modified) {
        LOG.debug("after replacing \\, with , in CATEGORIES:\n%s", data.c_str());
    }
    
    return data;
}

string EvolutionCalendarSource::ItemID::getLUID() const
{
    return getLUID(m_uid, m_rid);
}

string EvolutionCalendarSource::ItemID::getLUID(const string &uid, const string &rid)
{
    return uid + "-rid" + rid;
}

EvolutionCalendarSource::ItemID EvolutionCalendarSource::ItemID::parseLUID(const string &luid)
{
    size_t ridoff = luid.rfind("-rid");
    if (ridoff != luid.npos) {
        return ItemID(luid.substr(0, ridoff),
                      luid.substr(ridoff + strlen("-rid")));
    } else {
        return ItemID(luid, "");
    }
}

EvolutionCalendarSource::ItemID EvolutionCalendarSource::getItemID(ECalComponent *ecomp)
{
    icalcomponent *icomp = e_cal_component_get_icalcomponent(ecomp);
    if (!icomp) {
        throwError("internal error in getItemID(): ECalComponent without icalcomp");
    }
    return getItemID(icomp);
}

EvolutionCalendarSource::ItemID EvolutionCalendarSource::getItemID(icalcomponent *icomp)
{
    const char *uid;
    struct icaltimetype rid;

    uid = icalcomponent_get_uid(icomp);
    rid = icalcomponent_get_recurrenceid(icomp);
    return ItemID(uid ? uid : "",
                  icalTime2Str(rid));
}

string EvolutionCalendarSource::getItemModTime(ECalComponent *ecomp)
{
    struct icaltimetype *modTime;
    e_cal_component_get_last_modified(ecomp, &modTime);
    eptr<struct icaltimetype, struct icaltimetype, EvolutionUnrefFree<struct icaltimetype> > modTimePtr(modTime, "item without modification time");
    return icalTime2Str(*modTimePtr);
}

string EvolutionCalendarSource::getItemModTime(const ItemID &id)
{
    eptr<icalcomponent> icomp(retrieveItem(id));
    icalproperty *lastModified = icalcomponent_get_first_property(icomp, ICAL_LASTMODIFIED_PROPERTY);
    if (!lastModified) {
        throwError("getItemModTime(): item without modification time");
    }
    struct icaltimetype modTime = icalproperty_get_lastmodified(lastModified);
    return icalTime2Str(modTime);

}

string EvolutionCalendarSource::icalTime2Str(const icaltimetype &tt)
{
    static const struct icaltimetype null = { 0 };
    if (!memcmp(&tt, &null, sizeof(null))) {
        return "";
    } else {
        eptr<char> timestr(ical_strdup(icaltime_as_ical_string(tt)));
        if (!timestr) {
            throwError("cannot convert to time string");
        }
        return timestr.get();
    }
}

#endif /* ENABLE_ECAL */

#ifdef ENABLE_MODULES
# include "EvolutionCalendarSourceRegister.cpp"
#endif