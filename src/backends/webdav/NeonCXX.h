/*
 * Copyright (C) 2010 Patrick Ohly <patrick.ohly@gmx.de>
 */

/**
 * Simplifies usage of neon in C++ by wrapping some calls in C++
 * classes. Includes all neon header files relevant for the backend.
 */

#ifndef INCL_NEONCXX
#define INCL_NEONCXX

#include <ne_session.h>
#include <ne_utils.h>
#include <ne_basic.h>
#include <ne_props.h>
#include <ne_request.h>

#include <string>
#include <list>

#include <boost/shared_ptr.hpp>
#include <boost/function.hpp>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

namespace Neon {
#if 0
}
#endif

/** comma separated list of features supported by libneon in use */
std::string features();

class Request;

class Settings {
 public:
    /**
     * base URL for WebDAV service
     */
    virtual std::string getURL() = 0;

    /**
     * host name must match for SSL?
     */
    virtual bool verifySSLHost() = 0;

    /**
     * SSL certificate must be valid?
     */
    virtual bool verifySSLCertificate() = 0;

    /**
     * proxy URL, empty for system default
     */
    virtual std::string proxy() = 0;

    /**
     * fill in username and password for specified realm (URL?),
     * throw error if not available
     */
    virtual void getCredentials(const std::string &realm,
                                std::string &username,
                                std::string &password) = 0;

    /**
     * standard SyncEvolution log level, see
     * Session::Session() how that is mapped to neon debugging
     */
    virtual int logLevel() = 0;

    /**
     * if true, then manipulate SEQUENCE and LAST-MODIFIED properties
     * so that Google CalDAV server accepts updates
     */
    virtual bool googleUpdateHack() const = 0;

    /**
     * if true, then avoid RECURRENCE-ID in sub items without
     * corresponding parent by replacing it with
     * X-SYNCEVOLUTION-RECURRENCE-ID
     */
    virtual bool googleChildHack() const = 0;

    /**
     * if true, then check whether server has added an unwanted alarm
     * and resend to get rid of it
     */
    virtual bool googleAlarmHack() const = 0;

    /**
     * duration in seconds after which communication with a server
     * fails with a timeout error; <= 0 picks a large default value
     */
    virtual int timeoutSeconds() const = 0;

    /**
     * use this to create a boost_shared pointer for a
     * Settings instance which needs to be freed differently
     */
    struct NullDeleter {
        void operator()(Settings *) const {}
    };
};

struct URI {
    std::string m_scheme;
    std::string m_host;
    std::string m_userinfo;
    unsigned int m_port;
    std::string m_path;
    std::string m_query;
    std::string m_fragment;

    /**
     * Split URL into parts. Throws TransportAgentException on
     * invalid url.  Port will be set to default for scheme if not set
     * in URL. Path is normalized.
     */
    static URI parse(const std::string &url);

    static URI fromNeon(const ne_uri &other);

    /**
     * produce new URI from current path and new one (may be absolute
     * and relative)
     */
    URI resolve(const std::string &path) const;

    /** compose URL from parts */
    std::string toURL() const;

    /**
     * URL-escape string
     */
    static std::string escape(const std::string &text);
    static std::string unescape(const std::string &text);

    /**
     * Removes differences caused by escaping different characters.
     * Appends slash if path is a collection (or meant to be one) and
     * doesn't have a trailing slash.
     */
    static std::string normalizePath(const std::string &path, bool collection);

    bool operator == (const URI &other) {
        return m_scheme == other.m_scheme &&
        m_host == other.m_host &&
        m_userinfo == other.m_userinfo &&
        m_port == other.m_port &&
        m_path == other.m_path &&
        m_query == other.m_query &&
        m_fragment == other.m_fragment;
    }
};

/** produce debug string for status, which may be NULL */
std::string Status2String(const ne_status *status);

/**
 * Wraps all session related activities.
 * Throws transport errors for fatal problems.
 */
class Session {
    /**
     * @param settings    must provide information about settings on demand
     */
    Session(const boost::shared_ptr<Settings> &settings);
    static boost::shared_ptr<Session> m_cachedSession;

 public:
    /**
     * Create or reuse Session instance.
     * 
     * One Session instance is kept alive throughout the life of the process,
     * to reuse proxy information (libproxy has a considerably delay during
     * initialization) and HTTP connection/authentication.
     */
    static boost::shared_ptr<Session> create(const boost::shared_ptr<Settings> &settings);
    ~Session();

#ifdef HAVE_LIBNEON_OPTIONS
    /** ne_options2() for a specific path*/
    unsigned int options(const std::string &path);
#endif

    /**
     * called with URI and complete result set; exceptions are logged, but ignored
     */
    typedef boost::function<void (const URI &, const ne_prop_result_set *)> PropfindURICallback_t;

    /**
     * called with URI and specific property, value string may be NULL (error case);
     * exceptions are logged and abort iterating over properties (but not URIs)
     */
    typedef boost::function<void (const URI &, const ne_propname *, const char *, const ne_status *)> PropfindPropCallback_t;

    /** ne_simple_propfind(): invoke callback for each URI */
    void propfindURI(const std::string &path, int depth,
                  const ne_propname *props,
                  const PropfindURICallback_t &callback);

    /** ne_simple_propfind(): invoke callback for each property of each URI */
    void propfindProp(const std::string &path, int depth,
                      const ne_propname *props,
                      const PropfindPropCallback_t &callback);

    /** URL which is in use */
    std::string getURL() const { return m_uri.toURL(); }

    /** same as getURL() split into parts */
    const URI &getURI() const { return m_uri; }

    /**
     * to be called after each operation which might have produced debugging output by neon;
     * automatically called by check()
     */
    void flush();

    /** throw error if error code indicates failure */
    void check(int error);

    ne_session *getSession() const { return m_session; }

    /**
     * time when last successul request completed, must be maintained by Request::run()
     */
    time_t getLastRequestEnd() const { return m_lastRequestEnd; }
    void setLastRequestEnd(time_t end) { m_lastRequestEnd = end; }

 private:
    boost::shared_ptr<Settings> m_settings;
    bool m_debugging;
    ne_session *m_session;
    URI m_uri;
    std::string m_proxyURL;
    time_t m_lastRequestEnd;

    /** ne_set_server_auth() callback */
    static int getCredentials(void *userdata, const char *realm, int attempt, char *username, char *password) throw();

    /** ne_ssl_set_verify() callback */
    static int sslVerify(void *userdata, int failures, const ne_ssl_certificate *cert) throw();

    /** ne_props_result callback which invokes a PropfindURICallback_t as user data */
    static void propsResult(void *userdata, const ne_uri *uri,
                            const ne_prop_result_set *results) throw();

    /** iterate over properties in result set */
    static void propsIterate(const URI &uri, const ne_prop_result_set *results,
                             const PropfindPropCallback_t &callback);

    /** ne_propset_iterator callback which invokes pair<URI, PropfindPropCallback_t> */
    static int propIterator(void *userdata,
                            const ne_propname *pname,
                            const char *value,
                            const ne_status *status) throw();

    // use pointers here, g++ 4.2.3 has issues with references (which was used before)
    typedef std::pair<const URI *, const PropfindPropCallback_t *> PropIteratorUserdata_t;
};

/**
 * encapsulates a ne_xml_parser
 */
class XMLParser
{
 public:
    XMLParser();
    ~XMLParser();

    ne_xml_parser *get() const { return m_parser; }

    /**
     * See ne_xml_startelm_cb:
     * arguments are parent state, namespace, name, attributes (NULL terminated)
     * @return < 0 abort, 0 decline, > 0 accept
     */
    typedef boost::function<int (int, const char *, const char *, const char **)> StartCB_t;

    /**
     * See ne_xml_cdata_cb:
     * arguments are state of element, data and data len
     * May be NULL.
     * @return != 0 to abort
     */
    typedef boost::function<int (int, const char *, size_t)> DataCB_t;

    /**
     * See ne_xml_endelm_cb:
     * arguments are state of element, namespace, name
     * May be NULL.
     * @return != 0 to abort
     */
    typedef boost::function<int (int, const char *, const char *)> EndCB_t;

    /**
     * add new handler, see ne_xml_push_handler()
     */
    XMLParser &pushHandler(const StartCB_t &start,
                           const DataCB_t &data = DataCB_t(),
                           const EndCB_t &end = EndCB_t());

    /**
     * StartCB_t: accepts a new element if namespace and name match
     */
    static int accept(const std::string &nspaceExpected,
                      const std::string &nameExpected,
                      const char *nspace,
                      const char *name);

    /**
     * DataCB_t: append to std::string
     */
    static int append(std::string &buffer,
                      const char *data,
                      size_t len);

    /**
     * EndCB_t: clear std::string
     */
    static int reset(std::string &buffer);

    /**
     * Setup parser for handling REPORT result.
     * Caller still needs to push a handler for
     * "urn:ietf:params:xml:ns:caldav", "calendar-data".
     * 
     * @param href      caller's buffer for DAV:href
     * @param etag      caller's buffer for DAV:getetag
     */
    void initReportParser(std::string &href,
                          std::string &etag);

 private:
    ne_xml_parser *m_parser;
    struct Callbacks {
        Callbacks(const StartCB_t &start,
                  const DataCB_t &data = DataCB_t(),
                  const EndCB_t &end = EndCB_t()) :
            m_start(start),
            m_data(data),
            m_end(end)
        {}
        StartCB_t m_start;
        DataCB_t m_data;
        EndCB_t m_end;
    };
    std::list<Callbacks> m_stack;

    static int startCB(void *userdata, int parent,
                       const char *nspace, const char *name,
                       const char **atts);
    static int dataCB(void *userdata, int state,
                      const char *cdata, size_t len);
    static int endCB(void *userdata, int state, 
                     const char *nspace, const char *name);

};

/**
 * encapsulates a ne_request, with std::string as read and write buffer
 */
class Request
{
 public:
    /**
     * read and write buffers owned by caller
     */
    Request(Session &session,
            const std::string &method,
            const std::string &path,
            const std::string &body,
            std::string &result);
    /**
     * read from buffer (owned by caller!) and
     * parse result as XML
     */
    Request(Session &session,
            const std::string &method,
            const std::string &path,
            const std::string &body,
            XMLParser &parser);
    ~Request();

    void setFlag(ne_request_flag flag, int value) { ne_set_request_flag(m_req, flag, value); }
    void addHeader(const std::string &name, const std::string &value) {
        ne_add_request_header(m_req, name.c_str(), value.c_str());
    }
    void run();
    std::string getResponseHeader(const std::string &name) {
        const char *value = ne_get_response_header(m_req, name.c_str());
        return value ? value : "";
    }
    int getStatusCode() { return ne_get_status(m_req)->code; }
    const ne_status *getStatus() { return ne_get_status(m_req); }

 private:
    Session &m_session;
    ne_request *m_req;
    std::string *m_result;
    XMLParser *m_parser;

    /** ne_block_reader implementation */
    static int addResultData(void *userdata, const char *buf, size_t len);

    /** throw error if error code *or* current status indicates failure */
    void check(int error);
};

}
SE_END_CXX

#endif // INCL_NEONCXX
