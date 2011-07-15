/*
 * Copyright (C) 2010 Intel Corporation
 */

#include "config.h"

#ifdef ENABLE_DAV

// include first, it sets HANDLE_LIBICAL_MEMORY for us
#include <syncevo/icalstrdup.h>

#include "CalDAVSource.h"

#include <boost/bind.hpp>
#include <boost/algorithm/string/replace.hpp>

#include <syncevo/declarations.h>
SE_BEGIN_CXX


CalDAVSource::CalDAVSource(const SyncSourceParams &params,
                           const boost::shared_ptr<Neon::Settings> &settings) :
    WebDAVSource(params, settings)
{
    SyncSourceLogging::init(InitList<std::string>("SUMMARY") + "LOCATION",
                            ", ",
                            m_operations);
    // override default backup/restore from base class with our own
    // version
    m_operations.m_backupData = boost::bind(&CalDAVSource::backupData,
                                            this, _1, _2, _3);
    m_operations.m_restoreData = boost::bind(&CalDAVSource::restoreData,
                                             this, _1, _2, _3);
}

void CalDAVSource::listAllSubItems(SubRevisionMap_t &revisions)
{
    revisions.clear();

    const std::string query =
        "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n"
        "<C:calendar-query xmlns:D=\"DAV:\"\n"
        "xmlns:C=\"urn:ietf:params:xml:ns:caldav\">\n"
        "<D:prop>\n"
        "<D:getetag/>\n"

        // In practice, peers always return the full data dump
        // even if asked to return only a subset. Therefore we use this
        // REPORT to populate our m_cache instead of sending lots of GET
        // requests later on: faster sync, albeit with higher
        // memory consumption.
        //
        // Because incremental syncs typically don't use listAllSubItems(),
        // this looks like a good trade-off.
#ifdef SHORT_ALL_SUB_ITEMS_DATA
        "<C:calendar-data>\n"
        "<C:comp name=\"VCALENDAR\">\n"
        "<C:prop name=\"VERSION\"/>\n"
        "<C:comp name=\"VEVENT\">\n"
        "<C:prop name=\"SUMMARY\"/>\n"
        "<C:prop name=\"UID\"/>\n"
        "<C:prop name=\"RECURRENCE-ID\"/>\n"
        "<C:prop name=\"SEQUENCE\"/>\n"
        "</C:comp>\n"
        "<C:comp name=\"VTIMEZONE\"/>\n"
        "</C:comp>\n"
        "</C:calendar-data>\n"
#else
        "<C:calendar-data/>\n"
#endif

        "</D:prop>\n"
        // filter expected by Yahoo! Calendar
        "<C:filter>\n"
        "<C:comp-filter name=\"VCALENDAR\">\n"
        "<C:comp-filter name=\"VEVENT\">\n"
        "</C:comp-filter>\n"
        "</C:comp-filter>\n"
        "</C:filter>\n"
        "</C:calendar-query>\n";
    Timespec deadline = createDeadline();
    getSession()->startOperation("REPORT 'meta data'", deadline);
    while (true) {
        string data;
        Neon::XMLParser parser;
        parser.initReportParser(boost::bind(&CalDAVSource::appendItem, this,
                                            boost::ref(revisions),
                                            _1, _2, boost::ref(data)));
        m_cache.clear();
        parser.pushHandler(boost::bind(Neon::XMLParser::accept, "urn:ietf:params:xml:ns:caldav", "calendar-data", _2, _3),
                           boost::bind(Neon::XMLParser::append, boost::ref(data), _2, _3));
        Neon::Request report(*getSession(), "REPORT", getCalendar().m_path, query, parser);
        report.addHeader("Depth", "1");
        report.addHeader("Content-Type", "application/xml; charset=\"utf-8\"");
        if (report.run()) {
            break;
        }
    }

    m_cache.m_initialized = true;
}

void CalDAVSource::addResource(StringMap &items,
                               const std::string &href,
                               const std::string &etag)
{
    std::string davLUID = path2luid(Neon::URI::parse(href).m_path);
    items[davLUID] = ETag2Rev(etag);
}

void CalDAVSource::updateAllSubItems(SubRevisionMap_t &revisions)
{
    // list items to identify new, updated and removed ones
    const std::string query =
        "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n"
        "<C:calendar-query xmlns:D=\"DAV:\"\n"
        "xmlns:C=\"urn:ietf:params:xml:ns:caldav\">\n"
        "<D:prop>\n"
        "<D:getetag/>\n"
        "</D:prop>\n"
        // filter expected by Yahoo! Calendar
        "<C:filter>\n"
        "<C:comp-filter name=\"VCALENDAR\">\n"
        "<C:comp-filter name=\"VEVENT\">\n"
        "</C:comp-filter>\n"
        "</C:comp-filter>\n"
        "</C:filter>\n"
        "</C:calendar-query>\n";
    Timespec deadline = createDeadline();
    StringMap items;
    getSession()->startOperation("updateAllSubItems REPORT 'list items'", deadline);
    while (true) {
        string data;
        Neon::XMLParser parser;
        items.clear();
        parser.initReportParser(boost::bind(&CalDAVSource::addResource,
                                            this, boost::ref(items), _1, _2));
        Neon::Request report(*getSession(), "REPORT", getCalendar().m_path, query, parser);
        report.addHeader("Depth", "1");
        report.addHeader("Content-Type", "application/xml; charset=\"utf-8\"");
        if (report.run()) {
            break;
        }
    }

    // remove obsolete entries
    SubRevisionMap_t::iterator it = revisions.begin();
    while (it != revisions.end()) {
        SubRevisionMap_t::iterator next = it;
        ++next;
        if (items.find(it->first) == items.end()) {
            revisions.erase(it);
        }
        it = next;
    }

    // build list of new or updated entries,
    // copy others to cache
    m_cache.clear();
    std::list<std::string> mustRead;
    BOOST_FOREACH(const StringPair &item, items) {
        SubRevisionMap_t::iterator it = revisions.find(item.first);
        if (it == revisions.end() ||
            it->second.m_revision != item.second) {
            // read current information below
            mustRead.push_back(item.first);
        } else {
            // copy still relevant information
            addSubItem(it->first, it->second);
        }
    }

    // request dump of these items, add to cache and revisions
    //
    // Failures to find or read certain items will be
    // ignored. appendItem() will only be called for actually
    // retrieved items. This is partly intentional: Google is known to
    // have problems with providing all of its data via GET or the
    // multiget REPORT below. It returns a 404 error for items that a
    // calendar-query includes (see loadItem()).  Such items are
    // ignored it and thus will be silently skipped. This is not
    // perfect, but better than failing the sync.
    if (!mustRead.empty()) {
        std::stringstream buffer;
        buffer << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<C:calendar-multiget xmlns:D=\"DAV:\"\n"
            "   xmlns:C=\"urn:ietf:params:xml:ns:caldav\">\n"
            "<D:prop>\n"
            "   <D:getetag/>\n"
            "   <C:calendar-data/>\n"
            "</D:prop>\n";
        BOOST_FOREACH(const std::string &href, mustRead) {
            buffer << "<D:href>" << luid2path(href) << "</D:href>\n";
        }
        buffer << "</C:calendar-multiget>";
        std::string query = buffer.str();
        getSession()->startOperation("updateAllSubItems REPORT 'multiget new/updated items'", deadline);
        while (true) {
            string data;
            Neon::XMLParser parser;
            parser.initReportParser(boost::bind(&CalDAVSource::appendItem, this,
                                                boost::ref(revisions),
                                                _1, _2, boost::ref(data)));
            m_cache.clear();
            parser.pushHandler(boost::bind(Neon::XMLParser::accept, "urn:ietf:params:xml:ns:caldav", "calendar-data", _2, _3),
                               boost::bind(Neon::XMLParser::append, boost::ref(data), _2, _3));
            Neon::Request report(*getSession(), "REPORT", getCalendar().m_path,
                                 query, parser);
            report.addHeader("Depth", "1");
            report.addHeader("Content-Type", "application/xml; charset=\"utf-8\"");
            if (report.run()) {
                break;
            }
        }
    }
}

int CalDAVSource::appendItem(SubRevisionMap_t &revisions,
                             const std::string &href,
                             const std::string &etag,
                             std::string &data)
{
    // Ignore responses with no data: this is not perfect (should better
    // try to figure out why there is no data), but better than
    // failing.
    //
    // One situation is the response for the collection itself,
    // which comes with a 404 status and no data with Google Calendar.
    if (data.empty()) {
        return 0;
    }

    Event::unescapeRecurrenceID(data);
    eptr<icalcomponent> calendar(icalcomponent_new_from_string((char *)data.c_str()), // cast is a hack for broken definition in old libical
                                 "iCalendar 2.0");
    std::string davLUID = path2luid(Neon::URI::parse(href).m_path);
    SubRevisionEntry &entry = revisions[davLUID];
    entry.m_revision = ETag2Rev(etag);
    long maxSequence = 0;
    std::string uid;
    entry.m_subids.clear();
    for (icalcomponent *comp = icalcomponent_get_first_component(calendar, ICAL_VEVENT_COMPONENT);
         comp;
         comp = icalcomponent_get_next_component(calendar, ICAL_VEVENT_COMPONENT)) {
        std::string subid = Event::getSubID(comp);
        uid = Event::getUID(comp);
        long sequence = Event::getSequence(comp);
        if (sequence > maxSequence) {
            maxSequence = sequence;
        }
        entry.m_subids.insert(subid);
    }
    entry.m_uid = uid;

    if (!m_cache.m_initialized) {
        boost::shared_ptr<Event> event(new Event);
        event->m_DAVluid = davLUID;
        event->m_UID = uid;
        event->m_etag = entry.m_revision;
        event->m_subids = entry.m_subids;
        event->m_sequence = maxSequence;
#ifndef SHORT_ALL_SUB_ITEMS_DATA
        // we got a full data dump, use it
        for (icalcomponent *comp = icalcomponent_get_first_component(calendar, ICAL_VEVENT_COMPONENT);
             comp;
             comp = icalcomponent_get_next_component(calendar, ICAL_VEVENT_COMPONENT)) {
            // remove useless X-LIC-ERROR
            Event::icalClean(comp);
        }
        event->m_calendar = calendar;
#endif
        m_cache.insert(make_pair(davLUID, event));
    }

    // reset data for next item
    data.clear();
    return 0;
}

void CalDAVSource::addSubItem(const std::string &luid,
                              const SubRevisionEntry &entry)
{
    boost::shared_ptr<Event> &event = m_cache[luid];
    event.reset(new Event);
    event->m_DAVluid = luid;
    event->m_etag = entry.m_revision;
    event->m_UID = entry.m_uid;
    // We don't know sequence and last-modified. This
    // information will have to be filled in by loadItem()
    // when some operation on this event needs it.
    event->m_subids = entry.m_subids;
}

void CalDAVSource::setAllSubItems(const SubRevisionMap_t &revisions)
{
    if (!m_cache.m_initialized) {
        // populate our cache (without data) from the information cached
        // for us
        BOOST_FOREACH(const SubRevisionMap_t::value_type &subentry,
                      revisions) {
            addSubItem(subentry.first,
                       subentry.second);
        }
        m_cache.m_initialized = true;
    }
}

SubSyncSource::SubItemResult CalDAVSource::insertSubItem(const std::string &luid, const std::string &callerSubID,
                                                         const std::string &item)
{
    SubItemResult subres;

    // parse new event
    boost::shared_ptr<Event> newEvent(new Event);
    newEvent->m_calendar.set(icalcomponent_new_from_string((char *)item.c_str()), // hack for old libical
                             "parsing iCalendar 2.0");
    struct icaltimetype lastmodtime = icaltime_null_time();
    icalcomponent *firstcomp = NULL;
    for (icalcomponent *comp = firstcomp = icalcomponent_get_first_component(newEvent->m_calendar, ICAL_VEVENT_COMPONENT);
         comp;
         comp = icalcomponent_get_next_component(newEvent->m_calendar, ICAL_VEVENT_COMPONENT)) {
        std::string subid = Event::getSubID(comp);
        EventCache::iterator it;
        if (!luid.empty() &&
            (it = m_cache.find(luid)) != m_cache.end()) {
            // Additional sanity check: ensure that the expected UID is set.
            // Necessary if the peer we synchronize with (aka the local
            // data storage) doesn't support foreign UIDs. Maemo 5 calendar
            // backend is one example.
            Event::setUID(comp, it->second->m_UID);
            newEvent->m_UID = it->second->m_UID;
        } else {
            newEvent->m_UID = Event::getUID(comp);
            if (newEvent->m_UID.empty()) {
                // create new UID
                newEvent->m_UID = UUID();
                Event::setUID(comp, newEvent->m_UID);
            }
        }
        newEvent->m_sequence = Event::getSequence(comp);
        newEvent->m_subids.insert(subid);

        // set DTSTAMP to LAST-MODIFIED in replacement
        //
        // Needed because Google insists on replacing the original
        // DTSTAMP and checks it (409, "Can only store an event with
        // a newer DTSTAMP").
        //
        // According to RFC 2445, the property is set once when the
        // event is created for the first time. RFC 5545 extends this
        // and states that without a METHOD property (the case with
        // CalDAV), DTSTAMP is identical to LAST-MODIFIED, so Google
        // is right.
        icalproperty *dtstamp = icalcomponent_get_first_property(comp, ICAL_DTSTAMP_PROPERTY);
        if (dtstamp) {
            icalproperty *lastmod = icalcomponent_get_first_property(comp, ICAL_LASTMODIFIED_PROPERTY);
            if (lastmod) {
                lastmodtime = icalproperty_get_lastmodified(lastmod);
                icalproperty_set_dtstamp(dtstamp, lastmodtime);
            }
        }
    }
    if (newEvent->m_subids.size() != 1) {
        SE_THROW("new CalDAV item did not contain exactly one VEVENT");
    }
    std::string subid = *newEvent->m_subids.begin();

    // Determine whether we already know the merged item even though
    // our caller didn't.
    std::string davLUID = luid;
    std::string knownSubID = callerSubID;
    if (davLUID.empty()) {
        EventCache::iterator it = m_cache.findByUID(newEvent->m_UID);
        if (it != m_cache.end()) {
            davLUID = it->first;
            knownSubID = subid;
        }
    }

    if (davLUID.empty()) {
        // New VEVENT; should not be part of an existing merged item
        // ("meeting series").
        //
        // If another app created a resource with the same UID,
        // then two things can happen:
        // 1. server merges the data (Google)
        // 2. adding the item is rejected (standard compliant CalDAV server)
        //
        // If the UID is truely new, then
        // 3. the server may rename the item
        //
        // The following code deals with case 3 and also covers case
        // 1, but our usual Google workarounds (for example, no
        // patching of SEQUENCE) were not applied and thus sending the
        // item might fail.
        //
        // Case 2 is not currently handled and causes the sync to fail.
        // This is in line with the current design ("concurrency detected,
        // causes error, fixed by trying again in slow sync").
        InsertItemResult res;
        // Yahoo expects resource names to match UID + ".ics".
        std::string name = newEvent->m_UID + ".ics";
        std::string buffer;
        const std::string *data;
        if (!settings().googleChildHack() || subid.empty()) {
            // avoid re-encoding item data
            data = &item;
        } else {
            // sanitize item first: when adding child event without parent,
            // then the RECURRENCE-ID confuses Google
            eptr<char> icalstr(ical_strdup(icalcomponent_as_ical_string(newEvent->m_calendar)));
            buffer = icalstr.get();
            Event::escapeRecurrenceID(buffer);
            data = &buffer;
        }
        res = insertItem(name, *data, true);
        subres.m_mainid = res.m_luid;
        subres.m_uid = newEvent->m_UID;
        subres.m_subid = subid;
        subres.m_revision = res.m_revision;

        EventCache::iterator it = m_cache.find(res.m_luid);
        if (it != m_cache.end()) {
            // merge into existing Event
            Event &event = loadItem(*it->second);
            event.m_etag = res.m_revision;
            if (event.m_subids.find(subid) != event.m_subids.end()) {
                // was already in that item but caller didn't seem to know
                subres.m_merged = true;
            } else {
                // add to merged item
                event.m_subids.insert(subid);                
            }
            icalcomponent_merge_component(event.m_calendar,
                                          newEvent->m_calendar.release()); // function destroys merged calendar
        } else {
            // Google Calendar adds a default alarm each time a VEVENT is added
            // anew. Avoid that by resending our data if necessary (= no alarm set).
            if (settings().googleAlarmHack() &&
                !icalcomponent_get_first_component(firstcomp, ICAL_VALARM_COMPONENT)) {
                // add to cache, then update it
                newEvent->m_DAVluid = res.m_luid;
                newEvent->m_etag = res.m_revision;
                m_cache[newEvent->m_DAVluid] = newEvent;

                // potentially need to know sequence and mod time on server:
                // keep pointer (clears pointer in newEvent),
                // then get and parse new copy from server
                eptr<icalcomponent> calendar = newEvent->m_calendar;

                if (settings().googleUpdateHack()) {
                    loadItem(*newEvent);

                    // increment in original data
                    newEvent->m_sequence++;
                    newEvent->m_lastmodtime++;
                    Event::setSequence(firstcomp, newEvent->m_sequence);
                    icalproperty *lastmod = icalcomponent_get_first_property(firstcomp, ICAL_LASTMODIFIED_PROPERTY);
                    if (lastmod) {
                        lastmodtime = icaltime_from_timet(newEvent->m_lastmodtime, false);
                        icalproperty_set_lastmodified(lastmod, lastmodtime);
                    }
                    icalproperty *dtstamp = icalcomponent_get_first_property(firstcomp, ICAL_DTSTAMP_PROPERTY);
                    if (dtstamp) {
                        icalproperty_set_dtstamp(dtstamp, lastmodtime);
                    }
                    // re-encode below
                    data = &buffer;
                }
                bool mangleRecurrenceID = settings().googleChildHack() && !subid.empty();
                if (data == &buffer || mangleRecurrenceID) {
                    eptr<char> icalstr(ical_strdup(icalcomponent_as_ical_string(calendar)));
                    buffer = icalstr.get();
                }
                if (mangleRecurrenceID) {
                    Event::escapeRecurrenceID(buffer);
                }
                SE_LOG_DEBUG(NULL, NULL, "resending VEVENT to get rid of VALARM");
                res = insertItem(name, *data, true);
                newEvent->m_etag =
                    subres.m_revision = res.m_revision;
                newEvent->m_calendar = calendar;
            } else {
                // add to cache without further changes
                newEvent->m_DAVluid = res.m_luid;
                newEvent->m_etag = res.m_revision;
                m_cache[newEvent->m_DAVluid] = newEvent;
            }
        }
    } else {
        if (subid != knownSubID) {
            SE_THROW("new CalDAV item does not have right RECURRENCE-ID");
        }
        Event &event = loadItem(davLUID);
        // no changes expected yet, copy previous attributes
        subres.m_mainid = davLUID;
        subres.m_uid = event.m_UID;
        subres.m_subid = subid;
        subres.m_revision = event.m_etag;

        // Google hack: increase sequence number if smaller or equal to
        // sequence on server. Server rejects update otherwise.
        // See http://code.google.com/p/google-caldav-issues/issues/detail?id=26
        if (settings().googleUpdateHack()) {
            // always bump SEQ by one before PUT
            event.m_sequence++;
            if (newEvent->m_sequence < event.m_sequence) {
                // override in new event, existing ones will be updated below
                Event::setSequence(firstcomp, event.m_sequence);
            } else {
                // new event sequence is equal or higher, use that
                event.m_sequence = newEvent->m_sequence;
            }
        }

        // update cache: find old VEVENT and remove it before adding new one,
        // update last modified time of all other components
        icalcomponent *removeme = NULL;
        for (icalcomponent *comp = icalcomponent_get_first_component(event.m_calendar, ICAL_VEVENT_COMPONENT);
             comp;
             comp = icalcomponent_get_next_component(event.m_calendar, ICAL_VEVENT_COMPONENT)) {
            if (Event::getSubID(comp) == subid) {
                removeme = comp;
            } else if (settings().googleUpdateHack()) {
                // increase modification time stamps to that of the new item,
                // Google rejects the whole update otherwise
                if (!icaltime_is_null_time(lastmodtime)) {
                    icalproperty *dtstamp = icalcomponent_get_first_property(comp, ICAL_DTSTAMP_PROPERTY);
                    if (dtstamp) {
                        icalproperty_set_dtstamp(dtstamp, lastmodtime);
                    }
                    icalproperty *lastmod = icalcomponent_get_first_property(comp, ICAL_LASTMODIFIED_PROPERTY);
                    if (lastmod) {
                        icalproperty_set_lastmodified(lastmod, lastmodtime);
                    }
                }
                // set SEQ to the one increased above
                Event::setSequence(comp, event.m_sequence);
            }
        }
        if (davLUID != luid) {
            // caller didn't know final UID: if found, the tell him that
            // we merged the item for him, if not, then don't complain about
            // it not being found (like we do when the item should exist
            // but doesn't)
            if (removeme) {
                subres.m_merged = true;
                icalcomponent_remove_component(event.m_calendar, removeme);
            } else {
                event.m_subids.insert(subid);
            }
        } else {
            if (removeme) {
                // this is what we expect when the caller mentions the DAV LUID
                icalcomponent_remove_component(event.m_calendar, removeme);
            } else {
                // caller confused?!
                SE_THROW("event not found");
            }
        }

        icalcomponent_merge_component(event.m_calendar,
                                      newEvent->m_calendar.release()); // function destroys merged calendar
        eptr<char> icalstr(ical_strdup(icalcomponent_as_ical_string(event.m_calendar)));
        std::string data = icalstr.get();

        // Google gets confused when adding a child without parent,
        // replace in that case.
        bool haveParent = event.m_subids.find("") != event.m_subids.end();
        if (settings().googleChildHack() && !haveParent) {
            Event::escapeRecurrenceID(data);
        }

        // TODO: avoid updating item on server immediately?
        InsertItemResult res = insertItem(event.m_DAVluid, data, true);
        if (res.m_merged ||
            res.m_luid != event.m_DAVluid) {
            // should not merge with anything, if so, our cache was invalid
            SE_THROW("CalDAV item not updated as expected");
        }
        event.m_etag = res.m_revision;
        subres.m_revision = event.m_etag;
    }

    return subres;
}

void CalDAVSource::readSubItem(const std::string &davLUID, const std::string &subid, std::string &item)
{
    Event &event = loadItem(davLUID);
    if (event.m_subids.size() == 1) {
        // simple case: convert existing VCALENDAR
        if (*event.m_subids.begin() == subid) {
            eptr<char> icalstr(ical_strdup(icalcomponent_as_ical_string(event.m_calendar)));
            item = icalstr.get();
        } else {
            SE_THROW("event not found");
        }
    } else {
        // complex case: create VCALENDAR with just the VTIMEZONE definition(s)
        // and the one event, then convert that
        eptr<icalcomponent> calendar(icalcomponent_new(ICAL_VCALENDAR_COMPONENT), "VCALENDAR");
        for (icalcomponent *tz = icalcomponent_get_first_component(event.m_calendar, ICAL_VTIMEZONE_COMPONENT);
             tz;
             tz = icalcomponent_get_next_component(event.m_calendar, ICAL_VTIMEZONE_COMPONENT)) {
            eptr<icalcomponent> clone(icalcomponent_new_clone(tz), "VTIMEZONE");
            icalcomponent_add_component(calendar, clone.release());
        }
        bool found = false;
        for (icalcomponent *comp = icalcomponent_get_first_component(event.m_calendar, ICAL_VEVENT_COMPONENT);
             comp;
             comp = icalcomponent_get_next_component(event.m_calendar, ICAL_VEVENT_COMPONENT)) {
            if (Event::getSubID(comp) == subid) {
                eptr<icalcomponent> clone(icalcomponent_new_clone(comp), "VEVENT");
                icalcomponent_add_component(calendar, clone.release());
                found = true;
                break;
            }
        }
        if (!found) {
            SE_THROW("event not found");
        }
        eptr<char> icalstr(ical_strdup(icalcomponent_as_ical_string(calendar)));
        item = icalstr.get();
    }
}

void CalDAVSource::Event::escapeRecurrenceID(std::string &data)
{
    boost::replace_all(data,
                       "\nRECURRENCE-ID",
                       "\nX-SYNCEVOLUTION-RECURRENCE-ID");
}

void CalDAVSource::Event::unescapeRecurrenceID(std::string &data)
{
    boost::replace_all(data,
                       "\nX-SYNCEVOLUTION-RECURRENCE-ID",
                       "\nRECURRENCE-ID");
}

std::string CalDAVSource::removeSubItem(const string &davLUID, const std::string &subid)
{
    // find item in cache first, load only if it is not going to be
    // removed entirely
    Event &event = findItem(davLUID);

    if (event.m_subids.size() == 1) {
        // remove entire merged item, nothing will be left after removal
        if (*event.m_subids.begin() != subid) {
            SE_THROW("event not found");
        } else {
            try {
                removeItem(event.m_DAVluid);
            } catch (const TransportStatusException &ex) {
                if (ex.syncMLStatus() == 404) {
                    // Someone must have created a detached recurrence on
                    // the server without the master event - or the 
                    // item was already removed while the sync ran.
                    // Let's log the problem and ignore it.
                    Exception::log();
                } else if (ex.syncMLStatus() == 409 &&
                           strstr(ex.what(), "Can't delete a recurring event")) {
                    // Google CalDAV:
                    // HTTP/1.1 409 Can't delete a recurring event except on its organizer's calendar
                    //
                    // Workaround: remove RRULE and EXDATE before deleting
                    bool updated = false;
                    icalcomponent *comp = icalcomponent_get_first_component(event.m_calendar, ICAL_VEVENT_COMPONENT);
                    if (comp) {
                        icalproperty *prop;
                        while ((prop = icalcomponent_get_first_property(comp, ICAL_RRULE_PROPERTY)) != NULL) {
                            icalcomponent_remove_property(comp, prop);
                            updated = true;
                        }
                        while ((prop = icalcomponent_get_first_property(comp, ICAL_EXDATE_PROPERTY)) != NULL) {
                            icalcomponent_remove_property(comp, prop);
                            updated = true;
                        }
                    }
                    if (updated) {
                        SE_LOG_DEBUG(this, NULL, "Google recurring event delete hack: remove RRULE before deleting");
                        eptr<char> icalstr(ical_strdup(icalcomponent_as_ical_string(event.m_calendar)));
                        insertSubItem(davLUID, subid, icalstr.get());
                        // It has been observed that trying the DELETE immediately
                        // failed again with the same "Can't delete a recurring event"
                        // error although the event no longer has an RRULE. Seems
                        // that the Google server sometimes need a bit of time until
                        // changes really trickle through all databases. Let's
                        // try a few times before giving up.
                        for (int retry = 0; retry < 5; retry++) {
                            try {
                                SE_LOG_DEBUG(this, NULL, "Google recurring event delete hack: remove event, attempt #%d", retry);
                                removeSubItem(davLUID, subid);
                                break;
                            } catch (const TransportStatusException &ex2) {
                                if (ex2.syncMLStatus() == 409 &&
                                    strstr(ex2.what(), "Can't delete a recurring event")) {
                                    SE_LOG_DEBUG(this, NULL, "Google recurring event delete hack: try again in a second");
                                    sleep(1);
                                } else {
                                    throw;
                                }
                            }
                        }
                    } else {
                        SE_LOG_DEBUG(this, NULL, "Google recurring event delete hack not applicable, giving up");
                        throw;
                    }
                } else {
                    throw;
                }
            }
        }
        m_cache.erase(davLUID);
        return "";
    } else {
        loadItem(event);
        bool found = false;
        bool parentRemoved = false;
        for (icalcomponent *comp = icalcomponent_get_first_component(event.m_calendar, ICAL_VEVENT_COMPONENT);
             comp;
             comp = icalcomponent_get_next_component(event.m_calendar, ICAL_VEVENT_COMPONENT)) {
            if (Event::getSubID(comp) == subid) {
                icalcomponent_remove_component(event.m_calendar, comp);
                icalcomponent_free(comp);
                found = true;
                if (subid.empty()) {
                    parentRemoved = true;
                }
            }
        }
        if (!found) {
            SE_THROW("event not found");
        }
        event.m_subids.erase(subid);
        // TODO: avoid updating the item immediately
        eptr<char> icalstr(ical_strdup(icalcomponent_as_ical_string(event.m_calendar)));
        InsertItemResult res;
        if (parentRemoved && settings().googleChildHack()) {
            // Must avoid VEVENTs with RECURRENCE-ID in
            // event.m_calendar and the PUT request.  Brute-force
            // approach here is to encode as string, escape, and parse
            // again.
            string item = icalstr.get();
            Event::escapeRecurrenceID(item);
            event.m_calendar.set(icalcomponent_new_from_string((char *)item.c_str()), // hack for old libical
                                 "parsing iCalendar 2.0");
            res = insertItem(davLUID, item, true);
        } else {
            res = insertItem(davLUID, icalstr.get(), true);
        }
        if (res.m_merged ||
            res.m_luid != davLUID) {
            SE_THROW("unexpected result of removing sub event");
        }
        event.m_etag = res.m_revision;
        return event.m_etag;
    }
}

void CalDAVSource::flushItem(const string &davLUID)
{
    // TODO: currently we always flush immediately, so no need to send data here
    EventCache::iterator it = m_cache.find(davLUID);
    if (it != m_cache.end()) {
        it->second->m_calendar.set(NULL);
    }
}

std::string CalDAVSource::getSubDescription(const string &davLUID, const string &subid)
{
    Event &event = findItem(davLUID);
    if (!event.m_calendar) {
        // Don't load (expensive!) only to provide the description.
        // Returning an empty string will trigger the fallback (logging the ID).
        return "";
    }
    for (icalcomponent *comp = icalcomponent_get_first_component(event.m_calendar, ICAL_VEVENT_COMPONENT);
         comp;
         comp = icalcomponent_get_next_component(event.m_calendar, ICAL_VEVENT_COMPONENT)) {
        if (Event::getSubID(comp) == subid) {
            std::string descr;

            const char *summary = icalcomponent_get_summary(comp);
            if (summary && summary[0]) {
                descr += summary;
            }
        
            if (true /* is event */) {
                const char *location = icalcomponent_get_location(comp);
                if (location && location[0]) {
                    if (!descr.empty()) {
                        descr += ", ";
                    }
                    descr += location;
                }
            }
            // TODO: other item types
            return descr;
        }
    }
    return "";
}

std::string CalDAVSource::getDescription(const string &luid)
{
    StringPair ids = MapSyncSource::splitLUID(luid);
    return getSubDescription(ids.first, ids.second);
}

CalDAVSource::Event &CalDAVSource::findItem(const std::string &davLUID)
{
    EventCache::iterator it = m_cache.find(davLUID);
    if (it == m_cache.end()) {
        throwError("event not found");
    }
    return *it->second;
}

CalDAVSource::Event &CalDAVSource::loadItem(const std::string &davLUID)
{
    Event &event = findItem(davLUID);
    return loadItem(event);
}

CalDAVSource::Event &CalDAVSource::loadItem(Event &event)
{
    if (!event.m_calendar) {
        std::string item;
        try {
            readItem(event.m_DAVluid, item, true);
        } catch (const TransportStatusException &ex) {
            if (ex.syncMLStatus() == 404) {
                // Someone must have created a detached recurrence on
                // the server without the master event. We avoid that
                // with the "Google Child Hack", but have no control
                // over other clients. So let's deal with this problem
                // after logging it.
                Exception::log();

                // We know about the event because it showed up in a REPORT.
                // So let's use such a REPORT to retrieve the desired item.
                // Not as efficient as a GET (and thus not the default), but
                // so be it.
#if 0
                // This would be fairly efficient, but runs into the same 404 error as a GET.
                std::string query =
                    StringPrintf("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                                 "<C:calendar-multiget xmlns:D=\"DAV:\"\n"
                                 "   xmlns:C=\"urn:ietf:params:xml:ns:caldav\">\n"
                                 "<D:prop>\n"
                                 "   <C:calendar-data/>\n"
                                 "</D:prop>\n"
                                 "<D:href><[CDATA[%s]]></D:href>\n"
                                 "</C:calendar-multiget>",
                                 event.m_DAVluid.c_str());
                Neon::XMLParser parser;
                std::string href, etag;
                item = "";
                parser.initReportParser(href, etag);
                parser.pushHandler(boost::bind(Neon::XMLParser::accept, "urn:ietf:params:xml:ns:caldav", "calendar-data", _2, _3),
                                   boost::bind(Neon::XMLParser::append, boost::ref(item), _2, _3));
                Neon::Request report(*getSession(), "REPORT", getCalendar().m_path, query, parser);
                report.addHeader("Content-Type", "application/xml; charset=\"utf-8\"");
                report.run();
#else
                std::string query =
                    StringPrintf("<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n"
                                 "<C:calendar-query xmlns:D=\"DAV:\"\n"
                                 "xmlns:C=\"urn:ietf:params:xml:ns:caldav\">\n"
                                 "<D:prop>\n"
                                 "<D:getetag/>\n"
                                 "<C:calendar-data/>\n"
                                 "</D:prop>\n"
                                 // filter expected by Yahoo! Calendar
                                 "<C:filter>\n"
                                 "<C:comp-filter name=\"VCALENDAR\">\n"
                                 "<C:comp-filter name=\"VEVENT\">\n"
                                 "<C:prop-filter name=\"UID\">\n"
                                 "<C:text-match collation=\"i;octet\"><![CDATA[%s]]></C:text-match>\n"
                                 "</C:prop-filter>\n"
                                 "</C:comp-filter>\n"
                                 "</C:comp-filter>\n"
                                 "</C:filter>\n"
                                 "</C:calendar-query>\n",
                                 event.m_UID.c_str());
                Timespec deadline = createDeadline();
                getSession()->startOperation("REPORT 'single item'", deadline);
                while (true) {
                    Neon::XMLParser parser;
                    parser.initReportParser();
                    item = "";
                    parser.pushHandler(boost::bind(Neon::XMLParser::accept, "urn:ietf:params:xml:ns:caldav", "calendar-data", _2, _3),
                                       boost::bind(Neon::XMLParser::append, boost::ref(item), _2, _3));
                    Neon::Request report(*getSession(), "REPORT", getCalendar().m_path, query, parser);
                    report.addHeader("Depth", "1");
                    report.addHeader("Content-Type", "application/xml; charset=\"utf-8\"");
                    if (report.run()) {
                        break;
                    }
                }
#endif
            } else {
                throw;
            }
        }
        Event::unescapeRecurrenceID(item);
        event.m_calendar.set(icalcomponent_new_from_string((char *)item.c_str()), // hack for old libical
                             "parsing iCalendar 2.0");

        // Sequence number/last-modified might have been increased by last save.
        // Or the cache was populated by setAllSubItems(), which doesn't give
        // us the information. In that case, UID might also still be unknown.
        // Either way, check it again.
        for (icalcomponent *comp = icalcomponent_get_first_component(event.m_calendar, ICAL_VEVENT_COMPONENT);
             comp;
             comp = icalcomponent_get_next_component(event.m_calendar, ICAL_VEVENT_COMPONENT)) {
            if (event.m_UID.empty()) {
                event.m_UID = Event::getUID(comp);
            }
            long sequence = Event::getSequence(comp);
            if (sequence > event.m_sequence) {
                event.m_sequence = sequence;
            }
            icalproperty *lastmod = icalcomponent_get_first_property(comp, ICAL_LASTMODIFIED_PROPERTY);
            if (lastmod) {
                icaltimetype lastmodtime = icalproperty_get_lastmodified(lastmod);
                time_t mod = icaltime_as_timet(lastmodtime);
                if (mod > event.m_lastmodtime) {
                    event.m_lastmodtime = mod;
                }
            }

            // remove useless X-LIC-ERROR
            Event::icalClean(comp);
        }
    }
    return event;
}

void CalDAVSource::Event::icalClean(icalcomponent *comp)
{
    icalproperty *prop = icalcomponent_get_first_property(comp, ICAL_ANY_PROPERTY);
    while (prop) {
        icalproperty *next = icalcomponent_get_next_property(comp, ICAL_ANY_PROPERTY);
        const char *name = icalproperty_get_property_name(prop);
        if (name && !strcmp("X-LIC-ERROR", name)) {
            icalcomponent_remove_property(comp, prop);
            icalproperty_free(prop);
        }
        prop = next;
    }
}


std::string CalDAVSource::Event::icalTime2Str(const icaltimetype &tt)
{
    static const struct icaltimetype null = { 0 };
    if (!memcmp(&tt, &null, sizeof(null))) {
        return "";
    } else {
        eptr<char> timestr(ical_strdup(icaltime_as_ical_string(tt)));
        if (!timestr) {
            SE_THROW("cannot convert to time string");
        }
        return timestr.get();
    }
}

std::string CalDAVSource::Event::getSubID(icalcomponent *comp)
{
    struct icaltimetype rid = icalcomponent_get_recurrenceid(comp);
    return icalTime2Str(rid);
}

std::string CalDAVSource::Event::getUID(icalcomponent *comp)
{
    std::string uid;
    icalproperty *prop = icalcomponent_get_first_property(comp, ICAL_UID_PROPERTY);
    if (prop) {
        uid = icalproperty_get_uid(prop);
    }
    return uid;
}

void CalDAVSource::Event::setUID(icalcomponent *comp, const std::string &uid)
{
    icalproperty *prop = icalcomponent_get_first_property(comp, ICAL_UID_PROPERTY);
    if (prop) {
        icalproperty_set_uid(prop, uid.c_str());
    } else {
        icalcomponent_add_property(comp, icalproperty_new_uid(uid.c_str()));
    }
}

int CalDAVSource::Event::getSequence(icalcomponent *comp)
{
    int sequence = 0;
    icalproperty *prop = icalcomponent_get_first_property(comp, ICAL_SEQUENCE_PROPERTY);
    if (prop) {
        sequence = icalproperty_get_sequence(prop);
    }
    return sequence;
}

void CalDAVSource::Event::setSequence(icalcomponent *comp, int sequence)
{
    icalproperty *prop = icalcomponent_get_first_property(comp, ICAL_SEQUENCE_PROPERTY);
    if (prop) {
        icalproperty_set_sequence(prop, sequence);
    } else {
        icalcomponent_add_property(comp, icalproperty_new_sequence(sequence));
    }
}

CalDAVSource::EventCache::iterator CalDAVSource::EventCache::findByUID(const std::string &uid)
{
    for (iterator it = begin();
         it != end();
         ++it) {
        if (it->second->m_UID == uid) {
            return it;
        }
    }
    return end();
}

void CalDAVSource::backupData(const SyncSource::Operations::ConstBackupInfo &oldBackup,
                              const SyncSource::Operations::BackupInfo &newBackup,
                              BackupReport &backupReport)
{
    contactServer();

    // If this runs as part of the sync preparations, then we might
    // use the result to populate our m_cache. But because dumping
    // data is typically disabled, this optimization isn't really
    // worth that much.

    ItemCache cache;
    cache.init(oldBackup, newBackup, false);

    // stream directly from REPORT with full data into backup
    const std::string query =
        "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n"
        "<C:calendar-query xmlns:D=\"DAV:\"\n"
        "xmlns:C=\"urn:ietf:params:xml:ns:caldav\">\n"
        "<D:prop>\n"
        "<D:getetag/>\n"
        "<C:calendar-data/>\n"
        "</D:prop>\n"
        // filter expected by Yahoo! Calendar
        "<C:filter>\n"
        "<C:comp-filter name=\"VCALENDAR\">\n"
        "<C:comp-filter name=\"VEVENT\">\n"
        "</C:comp-filter>\n"
        "</C:comp-filter>\n"
        "</C:filter>\n"
        "</C:calendar-query>\n";
    string data;
    Neon::XMLParser parser;
    parser.initReportParser(boost::bind(&CalDAVSource::backupItem, this,
                                        boost::ref(cache),
                                        _1, _2, boost::ref(data)));
    parser.pushHandler(boost::bind(Neon::XMLParser::accept, "urn:ietf:params:xml:ns:caldav", "calendar-data", _2, _3),
                       boost::bind(Neon::XMLParser::append, boost::ref(data), _2, _3));
    Timespec deadline = createDeadline();
    getSession()->startOperation("REPORT 'full calendar'", deadline);
    while (true) {
        Neon::Request report(*getSession(), "REPORT", getCalendar().m_path, query, parser);
        report.addHeader("Depth", "1");
        report.addHeader("Content-Type", "application/xml; charset=\"utf-8\"");
        if (report.run()) {
            break;
        }
        cache.reset();
    }
    cache.finalize(backupReport);
}

int CalDAVSource::backupItem(ItemCache &cache,
                             const std::string &href,
                             const std::string &etag,
                             std::string &data)
{
    Event::unescapeRecurrenceID(data);
    std::string luid = path2luid(Neon::URI::parse(href).m_path);
    std::string rev = ETag2Rev(etag);
    cache.backupItem(data, luid, rev);

    // reset data for next item
    data.clear();
    return 0;
}

void CalDAVSource::restoreData(const SyncSource::Operations::ConstBackupInfo &oldBackup,
                               bool dryrun,
                               SyncSourceReport &report)
{
    // TODO: implement restore
    throw("not implemented");
}

bool CalDAVSource::typeMatches(const StringMap &props) const
{
    StringMap::const_iterator it = props.find("urn:ietf:params:xml:ns:caldav:supported-calendar-component-set");
    if (it != props.end() &&
        it->second.find("<urn:ietf:params:xml:ns:caldavcomp name='VEVENT'></urn:ietf:params:xml:ns:caldavcomp>") != std::string::npos) {
        return true;
    } else {
        return false;
    }
}

SE_END_CXX

#endif // ENABLE_DAV