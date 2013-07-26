//
// Copyright (c) 2012 Magnus Gille (mgille at gmail dot com)
//
//

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <boost/asio.hpp>
#include <boost/shared_ptr.hpp>


#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include "netlink.hpp"
#include "netlink.h"
#include "MacAddr.h"

/* I couldn't include linux/if.h */

#define IFF_LOWER_UP	0x10000

void statusChange(const std::string& eth,
              link_status prev,
              link_status now,
              void* contextData) {
    int* resolvedContext = reinterpret_cast<int*>(contextData);
    std::cout << eth << " changed from " << prev << " to " << now << std::endl;
    std::cout << "context is " << *resolvedContext << std::endl;
}

void macChange(const std::string& eth,
               MacAddr previousMac, MacAddr newMac, void* contextData) {
    int* resolvedContext = reinterpret_cast<int*>(contextData);
    std::cout << eth << " changed from " << previousMac.to_string() << " to " << newMac.to_string() << std::endl;
    std::cout << "context is " << *resolvedContext << std::endl;
}

int main(int argc, char* argv[])
{
    std::cout << "Starting" << std::endl;
    Netlink netlink;

    const int one=1;
    static const int two=2;
    netlink.registerStatusCallback(statusChange, (void*)&one);
    netlink.registerMacCallback(macChange, (void*)&two);

    std::cout << "Sleeping" << std::endl;
    sleep(60*2);
    std::cout << "Exiting" << std::endl;
    return 0;
}

class MonitorThread {
public:
    MonitorThread(Netlink& netlink)
        : _outerNetlink(netlink) 
    {}
    void operator()() ;
    
    Netlink& _outerNetlink;
};

void MonitorThread::operator()() {
    try
    {
        boost::asio::io_service io_service;
        boost::asio::basic_raw_socket<nl_protocol> s(io_service); 
        
        s.open(nl_protocol(NETLINK_ROUTE));
        //s.bind(nl_endpoint<nl_protocol>(RTMGRP_LINK|RTMGRP_NOTIFY|RTMGRP_IPV6_ROUTE, 0)); 
        s.bind(nl_endpoint<nl_protocol>(0xffffffff, 0)); 
        
        static const unsigned int max_length = 1024;
        char buffer[max_length];
        int bytes;
        
        while((bytes=s.receive(boost::asio::buffer(buffer, max_length)))) {
            boost::this_thread::interruption_point();
            
            for (nlmsghdr* nlm = (nlmsghdr *)buffer;
                 NLMSG_OK(nlm, (size_t)bytes);
                 nlm = NLMSG_NEXT(nlm, bytes))
            {
                _outerNetlink.handle_netlink(nlm);
            }
        }
    }
    catch (std::exception& e)
    {
        std::cerr << "Exception: " << e.what() << "\n";
    }
    
    return;
}


Netlink::Netlink()
    : _thread(MonitorThread(*this))
{
}

Netlink::~Netlink()
{
}

void Netlink::registerStatusCallback(StatusFn fn, void* contextData) {
    assert(_statusCB.fn == 0);
    _statusCB.fn = fn;
    _statusCB.contextData = contextData;
}

void Netlink::registerMacCallback(MacFn fn, void* contextData) {
    assert(_macCB.fn == 0);
    _macCB.fn = fn;
    _macCB.contextData = contextData;
}



void Netlink::handle_netlink(nlmsghdr *nlm) {
    
    if (nlm->nlmsg_type != RTM_NEWLINK && nlm->nlmsg_type != RTM_DELLINK) {
#if 0
        std::cout << "Uninteresting message type "
                  << nlm->nlmsg_type
                  << " (not RTM_NEWLINK, RTM_DELLINK)"
                  << std::endl;
#endif
        return;                                                                                               
    } 
#if 0    
    std::cout << "Interesting message type "
              << nlm->nlmsg_type
              << " (RTM_NEWLINK, RTM_DELLINK)"
              << std::endl;
#endif
    if (nlm->nlmsg_type == RTM_NEWLINK) {
        int len = nlm->nlmsg_len - sizeof(*nlm);
        if ((size_t)len < sizeof(ifinfomsg)) {
            errno = EBADMSG;
            return;
        }
        ifinfomsg *ifi = (ifinfomsg*)NLMSG_DATA(nlm);
        if (ifi->ifi_flags & IFF_LOOPBACK) {
            std::cout << "Flags show change refers to loopback - not interesting" 
                      << std::endl;
            return;
        }
	
        std::string ifname;
        MacAddr mac;
        link_status linkStatus;

        if((ifi->ifi_flags&IFF_LOWER_UP)==IFF_LOWER_UP) {
            linkStatus = up;
        } else {
            linkStatus = down;
        }

        rtattr *rta = (rtattr *) ((char *)ifi + NLMSG_ALIGN(sizeof(*ifi)));
        len = NLMSG_PAYLOAD(nlm, sizeof(*ifi));
        while (RTA_OK(rta, len)) {
            switch (rta->rta_type) {
                case IFLA_IFNAME:
                    ifname = reinterpret_cast<const char*>(RTA_DATA(rta));
                    break;
                case IFLA_ADDRESS:
                    mac = MacAddr(reinterpret_cast<unsigned char*>(RTA_DATA(rta)));
                    break;
            }
            rta = RTA_NEXT(rta, len);	    
        }
        
        checkMacChange(ifname, mac);
        checkStatusChange(ifname, linkStatus);

    }
    
}

void Netlink::checkStatusChange(const std::string& ifname, link_status linkStatus) {
    StatusMap::iterator statusIter = _trackStatus.find(ifname);
    if (_trackStatus.end() == statusIter) {
        if (_statusCB.fn != 0) {
            _statusCB.fn(ifname, unknown, linkStatus, _statusCB.contextData);            
        }
    } else {
        if (_statusCB.fn != 0) {
            if (statusIter->second != linkStatus) {
                _statusCB.fn(ifname, statusIter->second, linkStatus, _statusCB.contextData);            
            }
        }
    }
    _trackStatus[ifname]=linkStatus;            
}

void Netlink::checkMacChange(const std::string& ifname, const MacAddr& mac) {
    MacMap::iterator macIter = _trackMacAddr.find(ifname);
    if (_trackMacAddr.end() == macIter) {
        if (_macCB.fn != 0) {
            _macCB.fn(ifname, MacAddr::empty_addr(), mac, _macCB.contextData);            
        }
    } else {
        if (_macCB.fn != 0) {
            if (macIter->second != mac) {
                _macCB.fn(ifname, macIter->second, mac, _macCB.contextData);            
            }
        }
    }
    _trackMacAddr[ifname]=mac;            
}
