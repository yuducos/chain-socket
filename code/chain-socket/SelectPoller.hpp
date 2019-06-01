#ifndef SELECTPOLLER_H_
#define SELECTPOLLER_H_

#include "SystemReader.h"

#if defined(OS_WINDOWS)
#include "NetStruct.h"
#include "PrivateHeaders.h"

inline int getPollIndex(int id, int maxNum) {
#ifdef OS_WINDOWS
    int index = id / 4 % maxNum;
#else
    int index = id % maxNum;
#endif
    return index;
}

inline void PollInit() {
#ifdef OS_WINDOWS
    WORD ver = MAKEWORD(2, 2);
    WSADATA dat;
    WSAStartup(ver, &dat);
#else
    signal(SIGPIPE, SIG_IGN);
#endif
}

class SelectPoller {
protected:
    struct SocketData {
        int flag = 0;
        int fd = 0;
    };
public:
    SelectPoller(int maxEvent) : maxEvent(maxEvent) {

    }

    ~SelectPoller() {
    }

    int pollMod(int sockFd, int flag/*EVENT_READ | EVENT_WRITE | EVENT_ONCE*/) {
        SocketData x;
        bool oneshot = false;
        if (flag & EVENT_ONCE)
            oneshot = true;
        x.flag = flag;
        x.fd = sockFd;
        clients[sockFd] = x;
        return 0;
    }

    int pollAdd(int sockFd, int flag/*EVENT_READ | EVENT_WRITE | EVENT_ONCE*/) {
        SocketData x;
        bool oneshot = false;
        if (flag & EVENT_ONCE)
            oneshot = true;
        x.flag = flag;
        x.fd = sockFd;
        clients[sockFd] = x;
        return 0;
    }

    int pollDel(int sockFd) {
        if(clients.count(sockFd) > 0)
            clients.erase(sockFd);
        return 0;
    }

    int pollGetEvents(int maxEvent) {
        fd_set fdRead;
        fd_set fdWrite;
        fd_set fdExp;

        FD_ZERO(&fdRead);
        FD_ZERO(&fdWrite);
        FD_ZERO(&fdExp);

        uint64_t maxSock = 0;

        for (auto &E :clients) {
            int trigger = false;
            if (E.second.flag & EVENT_READ) {
                FD_SET(E.second.fd, &fdRead);
            }
            if (E.second.flag & EVENT_WRITE) {
                FD_SET(E.second.fd, &fdWrite);
            }

            if (maxSock < E.second.fd) {
                maxSock = E.second.fd;
            }
        }
        if (maxSock == 0) {
            return 0;
        }
        timeval t = {0, 0};
        int ret = select((int) maxSock + 1, &fdRead, &fdWrite, &fdExp, &t);
        if (ret < 0) {

#if !defined(OS_WINDOWS)
            std::cout << "err: select" << errno << std::endl;
#else
            std::cout << "err: select " << WSAGetLastError() << std::endl;
#endif
        }
        evClients.clear();
        for (auto &E :clients) {
            int trigger_flag = E.second.flag & EVENT_ONCE ? EVENT_ONCE : 0;
            if (E.second.flag & EVENT_READ && FD_ISSET(E.second.fd, &fdRead)) {
                trigger_flag |= EVENT_READ;
            }
            if (E.second.flag & EVENT_WRITE && FD_ISSET(E.second.fd, &fdWrite)) {
                trigger_flag |= EVENT_WRITE;
            }
            evClients.emplace_back(SocketData{trigger_flag, E.second.fd});
        }
        for(auto &E :evClients)
        {
            if(E.flag & EVENT_ONCE)
                clients.erase(E.fd);
        }
        return (int)evClients.size();
    }


    int getEventSocket(int index) {
        return evClients[index].fd;
    }

    int getEventTrigerFlag(int index) {
        return evClients[index].flag;
    }

public:
    int maxEvent = 0;
    int pollId = 0;
    std::map<int, SocketData> clients;
    std::vector<SocketData> evClients;
};


#endif
#endif /* SERVER_SELECTPOLL_H_ */