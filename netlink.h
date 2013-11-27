#ifndef NETLINK_H
#define NETLINK_H

#include <boost/thread/thread.hpp>
#include "MacAddr.h"
#include "IPAddress.h"

struct nlmsghdr;
enum link_status { unknown=0, up, down };
typedef void (*StatusFn)(const std::string& eth, link_status previousStatus, link_status newStatus, void* contextData);
typedef void (*MacFn)(const std::string& eth, MacAddr previousMac, MacAddr newMac, void* contextData);

struct  NeighborEntry {
    IPAddress ip;
    MacAddr mac;
    int  nud;
    bool router;
};

typedef std::list<NeighborEntry> NeighborList;

class Netlink {
public:
    Netlink();
    ~Netlink();

    NeighborList getNeighborTable();
    void registerStatusCallback(StatusFn, void*);
    void registerMacCallback(MacFn, void*);

 private:
    void handle_netlink(nlmsghdr *nlm);
    void checkStatusChange(const std::string& ifname, link_status linkStatus);
    void checkMacChange(const std::string& ifname, const MacAddr& mac);

 private:
    boost::thread _thread;
    friend class MonitorThread;

    struct StatusCB {
        StatusCB() 
        : fn(0), contextData(0) {}
        StatusFn fn;
        void* contextData;
    } _statusCB;

    struct MacCB {
        MacCB() 
        : fn(0), contextData(0) {}
        MacFn fn;
        void* contextData;
    } _macCB;

    typedef std::map<std::string, MacAddr> MacMap;
    MacMap _trackMacAddr;

    typedef std::map<std::string, link_status> StatusMap;
    StatusMap _trackStatus;

};


#endif
