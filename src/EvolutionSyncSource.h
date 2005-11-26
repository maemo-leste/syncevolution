/*
 * Copyright (C) 2005 Patrick Ohly
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

#ifndef INCL_EVOLUTIONSYNCSOURCE
#define INCL_EVOLUTIONSYNCSOURCE

#include <string>
#include <vector>
#include <list>
using namespace std;

#include <libedataserver/e-source.h>
#include <libedataserver/e-source-list.h>

#include <spds/SyncSource.h>
#include <spdm/ManagementNode.h>

/**
 * This class implements the functionality shared by
 * both EvolutionCalenderSource and EvolutionContactSource:
 * - handling of change IDs and URI
 * - finding the calender/contact backend
 * - default implementation of SyncSource interface
 *
 * The default interface assumes that the backend's
 * open() already finds all items as well as new/modified/deleted
 * ones and stores their UIDs in the respective lists.
 * Then the SyncItem iterators just walk through these lists,
 * creating new items via createItem().
 *
 * Error reporting is done via the Log class and this instance
 * then just tracks whether any error has occurred. If that is
 * the case, then the caller has to assume that syncing somehow
 * failed and a full sync is needed the next time.
 *
 * It also adds an Evolution specific interface:
 * - listing backend storages: getSyncBackends()
 */
class EvolutionSyncSource : public SyncSource
{
  public:
    /**
     * Creates a new Evolution sync source.
     *
     * @param    name        the named needed by SyncSource
     * @param    changeId    is used to track changes in the Evolution backend
     * @param    id          identifies the backend; not specifying it makes this instance
     *                       unusable for anything but listing backend databases
     */
    EvolutionSyncSource( const string name, const string &changeId, const string &id ) :
        SyncSource( name.c_str() ),
        m_allItems( *this, SYNC_STATE_NONE ),
        m_newItems( *this, SYNC_STATE_NEW ),
        m_updatedItems( *this, SYNC_STATE_UPDATED ),
        m_deletedItems( *this, SYNC_STATE_DELETED ),
        m_changeId( changeId ),
        m_hasFailed( false ),
        m_isModified( false ),
        m_id( id )
        {}
    virtual ~EvolutionSyncSource() {}

    struct source {
        source( const string &name, const string &uri ) :
            m_name( name ), m_uri( uri ) {}
        string m_name;
        string m_uri;
    };
    typedef vector<source> sources;
    
    /**
     * returns a list of all know sources for the kind of items
     * supported by this sync source
     */
    virtual sources getSyncBackends() = 0;

    /**
     * Actually opens the data source specified in the constructor,
     * will throw the normal exceptions if that fails. Should
     * not modify the state of the sync source: that can be deferred
     * until the server is also ready and beginSync() is called.
     */
    virtual void open() = 0;

    /**
     * Extract information for the item identified by UID
     * and store it in a new SyncItem. The caller must
     * free that item.
     *
     * @param uid      identifies the item
     * @param state    the state of the item
     */
    virtual SyncItem *createItem( const string &uid, SyncState state ) = 0;

    /**
     * closes the data source so that it can be reopened
     *
     * Just as open() it should not affect the state of
     * the database unless some previous action requires
     * it.
     */
    virtual void close() = 0;

    /**
     * resets the lists of all/new/updated/deleted items
     */
    void resetItems();

    /**
     * returns true iff some failure occured
     */
    bool hasFailed() { return m_hasFailed; }

    /** convenience function: copies item's data into string */
    static string getData(SyncItem& item);

    /**
     * convenience function: gets property as string class
     *
     * @return empty string if property not found, otherwise its value
     */
    static string getPropertyValue(ManagementNode &node, const string &property);

    /**
     * factory function for a EvolutionSyncSources that provides the
     * given mime type; for the other parameters see constructor
     *
     * @return NULL if no source can handle the given type
     */
    static EvolutionSyncSource *createSource(
        const string &name,
        const string &changeId,
        const string &id,
        const string &mimeType );
    
    //
    // default implementation of SyncSource iterators
    //
    virtual SyncItem* getFirstItem() { return m_allItems.start(); }
    virtual SyncItem* getNextItem() { return m_allItems.iterate(); }
    virtual SyncItem* getFirstNewItem() { return m_newItems.start(); }
    virtual SyncItem* getNextNewItem() { return m_newItems.iterate(); }
    virtual SyncItem* getFirstUpdatedItem() { return m_updatedItems.start(); }
    virtual SyncItem* getNextUpdatedItem() { return m_updatedItems.iterate(); }
    virtual SyncItem* getFirstDeletedItem() { return m_deletedItems.start(); }
    virtual SyncItem* getNextDeletedItem() { return m_deletedItems.iterate(); }
    virtual void setItemStatus(const char *key, int status);
	 
  protected:
    /**
     * searches the list for a source with the given uri or name
     *
     * @param list      a list previously obtained from Gnome
     * @param id        a string identifying the data source: either its name or uri
     * @return   pointer to source or NULL if not found
     */
    ESource *findSource( ESourceList *list, const string &id );

    /**
     * throw an exception after a Gnome action failed and
     * remember that this instance has failed
     *
     * @param action     a string describing what was attempted
     * @param gerror     if not NULL: a more detailed description of the failure,
     *                                will be freed
     */
    void throwError( const string &action, GError *gerror );

    const string m_changeId;
    const string m_id;

    class itemList : public list<string> {
        const_iterator m_it;
        EvolutionSyncSource &m_source;
        SyncState m_state;
        
      public:
        itemList( EvolutionSyncSource &source, SyncState state ) :
            m_source( source ),
            m_state( state )
        {}
        /** start iterating, return first item if available */
        SyncItem *start() {
            m_it = begin();
            return iterate();
        }
        /** return current item if available, step to next one */
        SyncItem *iterate() {
            if (m_it != end()) {
                const string &uid( *m_it );
                ++m_it;
                if (&m_source.m_deletedItems == this) {
                    // just tell caller the uid of the deleted item
                    return new SyncItem( uid.c_str() );
                } else {
                    // retrieve item with all its data
                    return m_source.createItem( uid, m_state );
                }
            } else {
                return NULL;
            }
        }
    };
    
    /** UIDs of all/all new/all updated/all deleted items */
    itemList m_allItems,
        m_newItems,
        m_updatedItems,
        m_deletedItems;

    /** keeps track of failure state */
    bool m_hasFailed;

    /** remembers whether items have been modified during the sync */
    bool m_isModified;
};

#endif // INCL_EVOLUTIONSYNCSOURCE
