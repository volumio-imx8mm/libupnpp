/* Copyright (C) 2006-2020 J.F.Dockes
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
#ifndef _DEVICE_H_X_INCLUDED_
#define _DEVICE_H_X_INCLUDED_

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <map>

#include "libupnpp/device/service.hxx"
#include "libupnpp/soaphelp.hxx"
#include "libupnpp/upnperrcodes.hxx"
#include "libupnpp/upnppexports.hxx"

namespace UPnPProvider {

class UpnpService;

typedef std::function<int (const UPnPP::SoapIncoming&, UPnPP::SoapOutgoing&)>
soapfun;

/** 
 * Base Device class.
 * 
 * The derived class mostly need to implement the readLibFile() method for 
 * retrieving misc XML description fragments and files.
 */
class UPNPP_API UpnpDevice {
public:

    /** Construct a device object. 
     *
     * The device is not started. This will be done by the startloop() or 
     * eventloop() call when everything is set up.
     *
     * @param deviceId uuid for device: "uuid:UUIDvalue"
     */
    UpnpDevice(const std::string& deviceId);

    /** Construct an embedded device.
     *
     * The device is not started. This will be done by the startloop() or 
     * eventloop() call when everything is set up.
     * @param rootdev if not null, the device description will be
     *    stored in the <devices> section of the root device (this device
     *    will be embedded). Else behave as the other constructor.
     *    !! The root device must not be already started. !!
     */
    UpnpDevice(UpnpDevice *rootdev, const std::string& deviceId);
    
    virtual ~UpnpDevice();

    /** Retrieve the network endpoint the server is listening on */
    static bool ipv4(std::string *host, unsigned short *port);

    /** Librarian utility.
     *
     * This must be implemented by the derived class for the services
     * to retrieve their definition XML files (by a call from the
     * base Service class constructor).
     *
     * This is also used with an empty name parameter to retrieve an 
     * XML text fragment to be added to the <device> node in the device 
     * description XML. E.G. things like <serialNumber>42</serialNumber>. 
     * *Mandatory*: deviceType and friendlyName *must* be in there, 
     * UDN *must not* (it is generated by the base Device class).
     * This empty name call will happen when the first service is
     * added, so the data should be prepared before this (e.g. if there are 
     * paths, like the one for presentation.html which need to be computed 
     * by addVFile(), this needs to happen before adding the first service).
     * 
     *  @param name the designator set in the service constructor 
     *      (e.g. AVTransport.xml). Empty for retrieving the description 
     *      properties (see above).
     *  @param[output] contents the output data.
     *  @return false for error.
     */
    virtual bool readLibFile(const std::string& name, std::string& contents) = 0;

    /** Add virtual file to virtual directory. 
     *
     *  This is mostly used internally by the base Service class to populate
     *  the virtual directory with data retrieved through the readLibFile() 
     *  method, but it can also be used by client code for serving other files 
     *  (e.g. the presentation page).
     *
     *  @param name Base file name. Somewhat arbitrary, but simple and 
     *      descriptive may help debugging (e.g. "presentation.html").
     *  @param contents File contents.
     *  @param mime MIME type (e.g: "text/html").
     *  @param[output] path Path chosen by the virtual directory.
     */
    bool addVFile(const std::string& name, const std::string& contents,
                  const std::string& mime, std::string& path);

    /**
     * Add mapping from service+action-name to handler function.
     * This is called by the services implementations during their 
     * initialization.
     */
    void addActionMapping(const UpnpService*,
                          const std::string& actName, soapfun);

    /** Retrieve Device ID (UDN) */
    const std::string& getDeviceId() const;
    
    /** Check status */
    bool ok();

    /**
     * Event-generating loop. 
     *
     * This can either be called from the main thread, or in a
     * separate thread by a call to startloop
     *
     * This loop mostly polls the derived class getEventData() method
     * and generates an UPnP event if it returns changed variables. 
     * 
     * The UPnP action calls happen in other (npupnp) threads with
     * which we synchronize, currently using a global lock.
     *
     * Alternatively to running this method, either directly or through 
     * startloop, it is possible to initially call start() (which returns 
     * after initializing the device with the lower level library), and then 
     * call notifyEvent() from the application own event loop.
     */
    void eventloop();

    /** 
     * Start a thread to run the event loop and return immediately. 
     *
     * This is an alternative to running eventloop() from
     * the main thread. The destructor will take care of the internal
     * thread.
     */
    void startloop();

    /**
     * Trigger an early event.
     *
     * This is called from a service action callback to wake up the
     * event loop early if something needs to be broadcast without
     * waiting for the normal delay.
     *
     * Will only do something if the previous event is not too recent.
     */
    void loopWakeup(); 

    /**
     * To be called to get the event loop to return
     */
    void shouldExit();

    /** 
     * Register and activate this device.
     *
     * This should only be called if eventloop() is not used, and any events
     * will be generated by calls to notifyEvent().
     */
    bool start();

     /** 
     * Generate an event for the service.
     * 
     * This is mostly useful if eventloop() is not used.
     */
    void notifyEvent(const UpnpService*,
                     const std::vector<std::string>& names,
                     const std::vector<std::string>& values);



    /* *******************************************************************
     * Methods called by the service constructor to link the
     * service object and its methods to the Device one, establishing
     * the eventing and action communication. This is internal and
     * should not be called by the library user. Not too sure how this
     * could/should be expressed in C++... */
    
    /** Add service to our list. 
     *
     * We only ever keep one instance of a serviceId. Multiple calls 
     * will only keep the last one. This is called from the generic 
     * UpnpService constructor.
     */
    bool addService(UpnpService *);
    void forgetService(const std::string& serviceId);

private:
    class UPNPP_LOCAL Internal;
    Internal *m;
    class InternalStatic;
    static InternalStatic *o;
};

} // End namespace UPnPProvider

#endif /* _DEVICE_H_X_INCLUDED_ */
