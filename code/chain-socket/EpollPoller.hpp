#include "SystemReader.h"

#if defined(OS_LINUX) && !defined(SELECT_SERVER)

#ifndef EPOLLPOLL_H_
#define EPOLLPOLL_H_

#include <sys/epoll.h>
#include <unistd.h>
#include "NetStruct.h"

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

class EpollPoller {
public:
    EpollPoller(int maxEvent) : maxEvent(maxEvent) {
        pollId = epoll_create(1);
        events = (struct epoll_event *) xmalloc(maxEvent * sizeof(struct epoll_event));
    }

    ~EpollPoller() {
        close(pollId);
        xfree(events);
    }

    int pollMod(int sockFd, int flag/*EVENT_READ | EVENT_WRITE | EVENT_ONCE*/) {
        ev1.data.fd = sockFd;
        ev1.events = flag;
        epoll_ctl(pollId, EPOLL_CTL_MOD, sockFd, &ev1);
        return 0;
    }

    int pollAdd(int sockFd, int flag/*EVENT_READ | EVENT_WRITE | EVENT_ONCE*/) {

        ev1.data.fd = sockFd;
        ev1.events = flag;

        return epoll_ctl(pollId, EPOLL_CTL_ADD, sockFd, &ev1);
    }

    int pollDel(int sockFd) {
        return 0;
    }

    int pollGetEvents(int maxEvent) {
        int numEvents = epoll_wait(pollId, events, maxEvent, 0);
        return numEvents;
    }

    int getEventSocket(int index) {
        return events[index].data.fd;
    }

    int getEventTrigerFlag(int index) {
        return events[index].events;
    }

public:
    int pollId = 0;
    struct epoll_event *events;
    struct epoll_event ev1;
    int maxEvent;
};


#endif /* SERVER_EPOLLPOLL_H_ */
#endif