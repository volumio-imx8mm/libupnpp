/* Copyright (C) 2006-2016 J.F.Dockes
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 *   02110-1301 USA
 */
#include "libupnpp/config.h"

#include "libupnpp/control/service.hxx"

#include <upnp/upnp.h>                  // for Upnp_Event, UPNP_E_SUCCESS, etc
#include <upnp/upnptools.h>             // for UpnpGetErrorMessage

#include <string>                       // for string, char_traits, etc
#include <utility>                      // for pair

#include "libupnpp/control/description.hxx"  // for UPnPDeviceDesc, etc
#include "libupnpp/ixmlwrap.hxx"
#include "libupnpp/log.hxx"             // for LOGDEB1, LOGINF, LOGERR, etc
#include "libupnpp/upnpp_p.hxx"         // for caturl
#include "libupnpp/upnpplib.hxx"        // for LibUPnP

using namespace std;
using namespace std::placeholders;
using namespace UPnPP;

#if UPNP_VERSION_MINOR < 8 && !defined(UpnpEvent_get_SID_cstr)
typedef struct Upnp_Event UpnpEvent;
#define UpnpEvent_get_SID_cstr(x) ((x)->Sid)
#define UpnpEvent_get_EventKey(x) ((x)->EventKey)
#define UpnpEvent_get_ChangedVariables(x) ((x)->ChangedVariables)
#endif

#if UPNP_VERSION_MAJOR > 1 || (UPNP_VERSION_MAJOR==1 && UPNP_VERSION_MINOR >= 8)
#define CBCONST const
#else
#define CBCONST 
#endif

namespace UPnPClient {
static bool initEvents();
static int srvCB(Upnp_EventType et, CBCONST void* vevp, void*);

// A small helper class for the functions which perform
// UpnpSendAction calls: get rid of IXML docs when done.
class IxmlCleaner {
public:
    IXML_Document **rqpp, **rspp;
    IxmlCleaner(IXML_Document** _rqpp, IXML_Document **_rspp)
        : rqpp(_rqpp), rspp(_rspp) {}
    ~IxmlCleaner()
    {
        if (*rqpp) ixmlDocument_free(*rqpp);
        if (*rspp) ixmlDocument_free(*rspp);
    }
};

class Service::Internal {
public:
    /** Upper level client code event callbacks. To be called by derived class
     * for reporting events. */
    VarEventReporter *reporter{nullptr};
    std::string actionURL;
    std::string eventURL;
    std::string serviceType;
    std::string deviceId;
    std::string friendlyName;
    std::string manufacturer;
    std::string modelName;
    Upnp_SID    SID{0}; /* Subscription Id */

    void initFromDeviceAndService(const UPnPDeviceDesc& devdesc,
                                  const UPnPServiceDesc& servdesc) {
        actionURL = caturl(devdesc.URLBase, servdesc.controlURL);
        eventURL = caturl(devdesc.URLBase, servdesc.eventSubURL);
        serviceType = servdesc.serviceType;
        deviceId = devdesc.UDN;
        friendlyName = devdesc.friendlyName;
        manufacturer = devdesc.manufacturer;
        modelName = devdesc.modelName;
    }
    /* Tell the UPnP device (through libupnp) that we want to receive
       its events. This is called by registerCallback() and sets m_SID */
    bool subscribe();
    bool unSubscribe();
};

/** Registered callbacks for all the service objects. The map is
 * indexed by SID, the subscription id which was obtained by
 * each object when subscribing to receive the events for its
 * device. The map allows the static function registered with
 * libupnp to call the appropriate object method when it receives
 * an event. */
static std::unordered_map<std::string, evtCBFunc> o_calls;


Service::Service(const UPnPDeviceDesc& devdesc,
                 const UPnPServiceDesc& servdesc)
{
    if ((m = new Internal()) == 0) {
        LOGERR("Device::Device: out of memory" << endl);
        return;
    }

    m->initFromDeviceAndService(devdesc, servdesc);
    // Only does anything the first time
    initEvents();
    // serviceInit() will be called from the derived class constructor
    // if needed
}

bool Service::initFromDescription(const UPnPDeviceDesc& devdesc)
{
    if (!m) {
        LOGERR("Device::Device: Internal is null" << endl);
        return false;
    }
    for (auto& servdesc : devdesc.services) {
        if (serviceTypeMatch(servdesc.serviceType)) {
            m->initFromDeviceAndService(devdesc, servdesc);
            // Only does anything the first time
            initEvents();
            return serviceInit(devdesc, servdesc);
        }
    }
    return false;
}

Service::Service()
{
    if ((m = new Internal()) == 0) {
        LOGERR("Device::Device: out of memory" << endl);
        return;
    }
}

Service::~Service()
{
    LOGDEB1("Service::~Service: " << m->eventURL << " SID " << m->SID << endl);
    unregisterCallback();
    delete m;
    m = 0;
}

const string& Service::getFriendlyName() const
{
    return m->friendlyName;
}

const string& Service::getDeviceId() const
{
    return m->deviceId;
}

const string& Service::getServiceType() const
{
    return m->serviceType;
}

const string& Service::getActionURL() const
{
    return m->actionURL;
}

const string& Service::getModelName() const
{
    return m->modelName;
}

const string& Service::getManufacturer() const
{
    return m->manufacturer;
}

int Service::runAction(const SoapOutgoing& args, SoapIncoming& data)
{
    LibUPnP* lib = LibUPnP::getLibUPnP();
    if (lib == 0) {
        LOGINF("Service::runAction: no lib" << endl);
        return UPNP_E_OUTOF_MEMORY;
    }
    UpnpClient_Handle hdl = lib->getclh();

    IXML_Document *request(0);
    IXML_Document *response(0);
    IxmlCleaner cleaner(&request, &response);

    if ((request = args.buildSoapBody(false)) == 0) {
        LOGINF("Service::runAction: buildSoapBody failed" << endl);
        return  UPNP_E_OUTOF_MEMORY;
    }

    LOGDEB1("Service::runAction: url [" << m->actionURL <<
           " serviceType " << m->serviceType <<
           " rqst: [" << ixmlwPrintDoc(request) << "]" << endl);

    int ret = UpnpSendAction(hdl, m->actionURL.c_str(), m->serviceType.c_str(),
                             0 /*devUDN*/, request, &response);

    if (ret != UPNP_E_SUCCESS) {
        if (ret < 0) {
            LOGINF("Service::runAction: UpnpSendAction failed: " << ret <<
                   " : " << UpnpGetErrorMessage(ret) << " for " <<
                   ixmlwPrintDoc(request) << endl);
        } else {
            // A remote error then
            SoapIncoming error;
            error.decode("UPnPError", response);
            int code = -1;
            string desc;
            error.get("errorCode", &code);
            error.get("errorDescription", &desc);
            LOGINF("Service::runAction: failed: errcode: " << code << " : \""
                   << desc << "\" for request: " << 
                   ixmlwPrintDoc(request) << endl);
        }
        return ret;
    }
    LOGDEB1("Service::runAction: rslt: [" <<
            ixmlwPrintDoc(response) << "]" << endl);

    if (!data.decode(args.getName().c_str(), response)) {
        LOGERR("Service::runAction: Could not decode response: " <<
               ixmlwPrintDoc(response) << endl);
        return UPNP_E_BAD_RESPONSE;
    }
    return UPNP_E_SUCCESS;
}

int Service::runTrivialAction(const std::string& actionName)
{
    SoapOutgoing args(m->serviceType, actionName);
    SoapIncoming data;
    return runAction(args, data);
}

template <class T> int Service::runSimpleGet(const std::string& actnm,
        const std::string& valnm,
        T *valuep)
{
    SoapOutgoing args(m->serviceType, actnm);
    SoapIncoming data;
    int ret = runAction(args, data);
    if (ret != UPNP_E_SUCCESS) {
        return ret;
    }
    if (!data.get(valnm.c_str(), valuep)) {
        LOGERR("Service::runSimpleAction: " << actnm <<
               " missing " << valnm << " in response" << std::endl);
        return UPNP_E_BAD_RESPONSE;
    }
    return 0;
}

template <class T> int Service::runSimpleAction(const std::string& actnm,
        const std::string& valnm,
        T value)
{
    SoapOutgoing args(m->serviceType, actnm);
    args(valnm, SoapHelp::val2s(value));
    SoapIncoming data;
    return runAction(args, data);
}

static std::mutex cblock;
// The static event callback given to libupnp
static int srvCB(Upnp_EventType et, CBCONST void* vevp, void*)
{
    std::unique_lock<std::mutex> lock(cblock);

    LOGDEB0("Service:srvCB: " << LibUPnP::evTypeAsString(et) << endl);

    switch (et) {
    case UPNP_EVENT_RENEWAL_COMPLETE:
    case UPNP_EVENT_SUBSCRIBE_COMPLETE:
    case UPNP_EVENT_UNSUBSCRIBE_COMPLETE:
    case UPNP_EVENT_AUTORENEWAL_FAILED:
    {
        const char *ff = (const char *)vevp;
        (void)ff;
        LOGDEB1("Service:srvCB: subs event: " << ff << endl);
        break;
    }

    case UPNP_EVENT_RECEIVED:
    {
        UpnpEvent *evp = (UpnpEvent *)vevp;
        LOGDEB1("Service:srvCB: var change event: SID " <<
                UpnpEvent_get_SID_cstr(evp) << " EventKey " <<
                UpnpEvent_get_EventKey(evp) << " changed " <<
                ixmlwPrintDoc(UpnpEvent_get_ChangedVariables(evp)) << endl);

        std::unordered_map<string, string> props;
        if (!decodePropertySet(UpnpEvent_get_ChangedVariables(evp), props)) {
            LOGERR("Service::srvCB: could not decode EVENT propertyset" <<endl);
            return UPNP_E_BAD_RESPONSE;
        }
        //for (auto& entry: props) {
        //LOGDEB("srvCB: " << entry.first << " -> " << entry.second << endl);
        //}

        auto it = o_calls.find(UpnpEvent_get_SID_cstr(evp));
        if (it != o_calls.end()) {
            (it->second)(props);
        } else {
            LOGINF("Service::srvCB: no callback found for sid " <<
                   UpnpEvent_get_SID_cstr(evp) << endl);
        }
        break;
    }

    default:
        // Ignore other events for now
        LOGDEB("Service:srvCB: unprocessed evt type: [" <<
               LibUPnP::evTypeAsString(et) << "]"  << endl);
        break;
    }

    return UPNP_E_SUCCESS;
}

// Only actually does something on the first call, to register our
// (static) library callback
static bool initEvents()
{
    LOGDEB1("Service::initEvents" << endl);

    std::unique_lock<std::mutex> lock(cblock);
    
    static bool eventinit(false);
    if (eventinit)
        return true;
    eventinit = true;

    LibUPnP *lib = LibUPnP::getLibUPnP();
    if (lib == 0) {
        LOGERR("Service::initEvents: Can't get lib" << endl);
        return false;
    }
    lib->registerHandler(UPNP_EVENT_RENEWAL_COMPLETE, srvCB, 0);
    lib->registerHandler(UPNP_EVENT_SUBSCRIBE_COMPLETE, srvCB, 0);
    lib->registerHandler(UPNP_EVENT_UNSUBSCRIBE_COMPLETE, srvCB, 0);
    lib->registerHandler(UPNP_EVENT_AUTORENEWAL_FAILED, srvCB, 0);
    lib->registerHandler(UPNP_EVENT_RECEIVED, srvCB, 0);
    return true;
}

bool Service::Internal::subscribe()
{
    LOGDEB1("Service::subscribe: " << eventURL << endl);
    LibUPnP* lib = LibUPnP::getLibUPnP();
    if (lib == 0) {
        LOGINF("Service::subscribe: no lib" << endl);
        return false;
    }
    int timeout = 1800;
    int ret = UpnpSubscribe(lib->getclh(), eventURL.c_str(),
                            &timeout, SID);
    if (ret != UPNP_E_SUCCESS) {
        LOGERR("Service:subscribe: failed: " << ret << " : " <<
               UpnpGetErrorMessage(ret) << endl);
        return false;
    }
    LOGDEB1("Service::subs:   " << eventURL << " SID " << SID << endl);
    return true;
}

bool Service::Internal::unSubscribe()
{
    LOGDEB1("Service::unSubs: " << m->eventURL << " SID " << m->SID << endl);
    LibUPnP* lib = LibUPnP::getLibUPnP();
    if (lib == 0) {
        LOGINF("Service::unSubscribe: no lib" << endl);
        return false;
    }
    if (SID[0]) {
        int ret = UpnpUnSubscribe(lib->getclh(), SID);
        if (ret != UPNP_E_SUCCESS) {
            LOGERR("Service:unSubscribe: failed: " << ret << " : " <<
                   UpnpGetErrorMessage(ret) << endl);
            return false;
        }
        // Let the caller erase m->SID[] because there may be other
        // cleanup to do, based on its value
    }
    return true;
}

void Service::registerCallback(evtCBFunc c)
{
    if (!m || !m->subscribe())
        return;
    std::unique_lock<std::mutex> lock(cblock);
    LOGDEB1("Service::registerCallback: " << m->eventURL << " SID " <<
            m->SID << endl);
    o_calls[m->SID] = c;
}

void Service::unregisterCallback()
{
    LOGDEB1("Service::unregisterCallback: " << m->eventURL << " SID " <<
            m->SID << endl);
    if (m->SID[0]) {
        m->unSubscribe();
        std::unique_lock<std::mutex> lock(cblock);
        o_calls.erase(m->SID);
        m->SID[0] = 0;
    }
}

VarEventReporter *Service::getReporter()
{
    if (m)
        return m->reporter;
    return nullptr;
}

void Service::installReporter(VarEventReporter* reporter)
{
    if (reporter) {
        registerCallback();
    } else {
        unregisterCallback();
    }
    m->reporter = reporter;
}

void Service::reSubscribe()
{
    LOGDEB("Service::reSubscribe()\n");
    if (m->SID[0] == 0) {
        LOGINF("Service::reSubscribe: no subscription (null SID)\n");
        return;
    }
    evtCBFunc c;
    {
        std::unique_lock<std::mutex> lock(cblock);
        auto it = o_calls.find(m->SID);
        if (it == o_calls.end()) {
            LOGINF("Service::reSubscribe: no callback found for m->SID " <<
                   m->SID << endl);
            return;
        }
        c = it->second;
    }
    unregisterCallback();
    registerCallback(c);
}

template int Service::runSimpleAction<int>(string const&, string const&, int);
template int Service::runSimpleAction<string>(string const&, string const&,
                                              string);
template int Service::runSimpleGet<int>(string const&, string const&, int*);
template int Service::runSimpleGet<bool>(string const&, string const&, bool*);
template int Service::runSimpleAction<bool>(string const&, string const&, bool);
template int Service::runSimpleGet<string>(string const&,string const&,string*);

}
