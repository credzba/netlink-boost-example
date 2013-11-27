#include <cstdlib>
#include <cstring>
#include <iostream>
#include <boost/asio.hpp>
#include <boost/shared_ptr.hpp>


#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>

#include "netlink.hpp"
#include "netlink.h"
#include <libnetlink.h>
#include "MacAddr.h"
#include "IPAddress.h"

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

    NeighborList neighborList = netlink.getNeighborTable();
    for (NeighborList::iterator iter=neighborList.begin();
         iter != neighborList.end();
         iter++) {
        std::cout << iter->ip.to_string() << " " << iter->mac.to_string() << " " << (iter->router ? "router" : "") << std::endl;
    }

    //netlink.registerStatusCallback(statusChange, (void*)&one);
    //netlink.registerMacCallback(macChange, (void*)&two);

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

int parse_rtattr(struct rtattr *tb[], int max, struct rtattr *rta, int len)
{
	memset(tb, 0, sizeof(struct rtattr *) * (max + 1));
	while (RTA_OK(rta, len)) {
		if ((rta->rta_type <= max) && (!tb[rta->rta_type]))
			tb[rta->rta_type] = rta;
		rta = RTA_NEXT(rta,len);
	}
	if (len)
		fprintf(stderr, "!!!Deficit %d, rta_len=%d\n", len, rta->rta_len);
	return 0;
}

bool get_neigh_info(const struct sockaddr_nl *who, struct nlmsghdr *n, NeighborList& result)
{
    NeighborEntry entry;
    ndmsg *r = reinterpret_cast<ndmsg *>(NLMSG_DATA(n));
    int len = n->nlmsg_len;
	struct rtattr * tb[NDA_MAX+1];
	char abuf[256];

	if (n->nlmsg_type != RTM_NEWNEIGH && n->nlmsg_type != RTM_DELNEIGH) {
            return false;
	}
	len -= NLMSG_LENGTH(sizeof(*r));
	if (len < 0) {
            //BUG: wrong nlmsg len 
            return false;
	}

	parse_rtattr(tb, NDA_MAX, NDA_RTA(r), n->nlmsg_len - NLMSG_LENGTH(sizeof(*r)));

	if (tb[NDA_DST]) {
            IPv6Address ip(*reinterpret_cast<boost::asio::ip::address_v6::bytes_type*>(RTA_DATA(tb[NDA_DST])));
            entry.ip = ip;
            //std::cout << ip.to_string() << std::endl;
            //std::cout << inet_ntop(r->ndm_family, RTA_DATA(tb[NDA_DST]), abuf, sizeof(abuf)) << std::endl;
	}
        // std::cout << "dev " << ll_index_to_name(r->ndm_ifindex);

	if (tb[NDA_LLADDR]) {
            int macLen = RTA_PAYLOAD(tb[NDA_LLADDR]);
            //if (macLen != 6) { std::cout << "length is " << macLen << std::endl; }
            MacAddr mac(reinterpret_cast<const u_int8_t*>(RTA_DATA(tb[NDA_LLADDR])));
            //std::cout << "mac addr " << mac.to_string() << std::endl;
            entry.mac = mac;
	}

	if (r->ndm_flags & NTF_ROUTER) {
            entry.router=true;
	} else {
            entry.router = false;
        }

	if (r->ndm_state) {
            entry.nud = r->ndm_state;
        } else {
            entry.nud = 0;
        }

    result.push_back(entry);
    return 0;
}


NeighborList Netlink::getNeighborTable() {
    NeighborList result;

    struct {
        struct nlmsghdr n;
        struct ifaddrmsg r;
    } req;

    rtattr *rta;
    int status;
    
    int fd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
    if (fd < 0) {
        perror("Cannot open netlink socket");
        return result;
    }

    int sndbuf = 32768;
    if (setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&sndbuf,sizeof(sndbuf)) < 0) {
        perror("SO_SNDBUF");
        return result;
    }

    int rcvbuf = 1024 * 1024;
    if (setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&rcvbuf,sizeof(rcvbuf)) < 0) {
        perror("SO_RCVBUF");
        return result;
    }

    struct sockaddr_nl	local;
    memset(&local, 0, sizeof(local));
    local.nl_family = AF_NETLINK;
    const unsigned int subscriptions = 0;
    local.nl_groups = subscriptions;

    if (bind(fd, (struct sockaddr*)&local, sizeof(local)) < 0) {
        perror("Cannot bind netlink socket");
        return result;
    }

    socklen_t addr_len = sizeof(local);
    if (getsockname(fd, (struct sockaddr*)&local, &addr_len) < 0) {
        perror("Cannot getsockname");
        return result;
    }
    if (addr_len != sizeof(local)) {
        fprintf(stderr, "Wrong address length %d\n", addr_len);
        return result;
    }
    if (local.nl_family != AF_NETLINK) {
        fprintf(stderr, "Wrong address family %d\n", local.nl_family);
	return result;
    }

    memset(&req, 0, sizeof(req));
    req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_ROOT;
    req.n.nlmsg_type = RTM_GETNEIGH;
    
    
    /* AF_INET6 is used to signify the kernel to fetch only ipv6 entires.         *
     * Replacing this with AF_INET will fetch ipv4 address table.                 */    
    req.r.ifa_family = AF_INET6;
    
    rta = (struct rtattr *)(((char *)&req) + NLMSG_ALIGN(req.n.nlmsg_len));
    rta->rta_len = RTA_LENGTH(16); // 16 = ipv6, 4 = ipv4 
    
    // send request for neighbor table
    status = send(fd, &req, req.n.nlmsg_len, 0);
    
    if (status < 0) {
        perror("send");
        return result;
    }

    struct sockaddr_nl nladdr;
    struct iovec iov;
    struct msghdr msg = {
        .msg_name = &nladdr,
        .msg_namelen = sizeof(nladdr),
        .msg_iov = &iov,
        .msg_iovlen = 1,
    };
    char buf[16384];
    iov.iov_base = buf;

    status = recvmsg(fd, &msg, 0);
    if (status < 0) {
        std::cout << "netlink receive error " << strerror(errno) << " " << errno << std::endl;
        return result;
    }
    if (status == 0) {
        std::cout << "end of file on netlink" << std::endl;
        return result;
    }

    struct nlmsghdr *h = (struct nlmsghdr*)buf;
    int msglen = status;
    while (NLMSG_OK(h, msglen)) {
        if (nladdr.nl_pid != 0 ||
            h->nlmsg_pid != local.nl_pid 
           ) {
            goto skip_it;
        }
        if (h->nlmsg_type == NLMSG_DONE) {
            break; /* process next filter */
        }
        if (h->nlmsg_type == NLMSG_ERROR) {
            struct nlmsgerr *err = (struct nlmsgerr*)NLMSG_DATA(h);
            if (h->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
                std::cout << "ERROR truncated" << std::endl;
            } else {
                errno = -err->error;
                perror("RTNETLINK answers");
            }
            return result;
        }
        get_neigh_info(&nladdr, h, result);
skip_it:
        h = NLMSG_NEXT(h, msglen);
    }
    
    return result;
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
